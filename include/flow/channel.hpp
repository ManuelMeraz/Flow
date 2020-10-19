#ifndef FLOW_CHANNEL_HPP
#define FLOW_CHANNEL_HPP

#include <algorithm>
#include <array>
#include <functional>
#include <thread>

#include <cppcoro/multi_producer_sequencer.hpp>
#include <cppcoro/sequence_barrier.hpp>
#include <cppcoro/static_thread_pool.hpp>
#include <cppcoro/sync_wait.hpp>
#include <cppcoro/task.hpp>
#include <cppcoro/when_all_ready.hpp>

#include <unordered_set>

#include "flow/atomic.hpp"
#include "flow/cancellation.hpp"
#include "flow/channel_status.hpp"
#include "flow/logging.hpp"
#include "flow/message.hpp"
#include "flow/spin_wait.hpp"

namespace flow {
/**
 * A channel represents the central location where all communication happens between different tasks
 * sharing information through message_registry
 *
 * It gets built up as publishers and subscribes are created by tasks. Once the begin phase is over, the open
 * communication coroutine begins and triggers all tasks to begin communication asynchronously.
 *
 * @tparam message_t The information being transmitted through this channel
 */
template<typename message_t, typename config_t>
class channel {
  using task_t = cppcoro::task<void>;
  using tasks_t = std::vector<task_t>;
  static constexpr std::size_t BUFFER_SIZE = config_t::channel::message_buffer_size;

public:
  using publisher_callback_t = flow::cancellable_function<void(message_t&)>;
  using subscriber_callback_t = flow::cancellable_function<void(message_t const&)>;
  using publisher_callbacks_t = std::vector<publisher_callback_t>;
  using subscriber_callbacks_t = std::vector<subscriber_callback_t>;

  channel(std::string name) : m_name(std::move(name)) {}

  /**
   * Called by channel_registry when handing over ownership of the callback registered by a task
   * @param callback The request or message call back from a task
   */
  void push_publisher(publisher_callback_t&& callback)
  {
    m_on_request_callbacks.push_back(std::move(callback));
  }
  void push_subscription(subscriber_callback_t&& callback)
  {
    m_on_message_callbacks.push_back(std::move(callback));
  }

  std::string_view name() const { return m_name; }

  std::size_t hash() { return typeid(message_t).hash_code() ^ std::hash<std::string>{}(m_name); }

  /**
   * Called as part of the main routine through flow::spin
   *
   * @param tp The threadpool created by flow::spin
   * @param io A scheduler for asynchronous io
   * @param barrier Handles signals for publishing/subscribing between tasks
   * @param sequencer Handles buffer organization of message_registry
   * @return A coroutine that will be executed by flow::spin
   */
  task_t open_communications([[maybe_unused]] auto& sched)
  {
    using namespace cppcoro;

    const auto message_buffer = [] {
      if constexpr (is_wrapped<message_t>()) {
        return std::array<message_t, BUFFER_SIZE>{};
      }
      else {
        return std::array<message<message_t>, BUFFER_SIZE>{};
      }
    };

    using buffer_t = decltype(message_buffer());
    using scheduler_t = decltype(sched);

    [[maybe_unused]] const std::size_t num_publishers = m_on_request_callbacks.size();
    [[maybe_unused]] const std::size_t num_subscribers = m_on_message_callbacks.size();

    struct context_t {
      scheduler_t& scheduler;
      channel_status status;

      sequence_barrier<std::size_t> barrier{};// notifies publishers that it can publish more
      multi_producer_sequencer<std::size_t> sequencer{ barrier, BUFFER_SIZE };// controls sequence values for the message buffer

      std::size_t index_mask = BUFFER_SIZE - 1;// Used to mask the sequence number and get the index to access the buffer
      buffer_t buffer{};// the message buffer
    } context{ sched, channel_status{ num_publishers, num_subscribers } };


    tasks_t merged = make_subscriber_tasks(context);
    tasks_t on_request_tasks = make_publisher_tasks(context);

    for (auto&& other : on_request_tasks) {
      merged.push_back(std::move(other));
    }

    co_await cppcoro::when_all_ready(std::move(merged));
  }

private:
  void invoke(auto& callback, auto& message)
  {
    if constexpr (is_wrapped<message_t>()) {
      std::invoke(callback, message);
    }
    else {
      std::invoke(callback, message.message);
    }
  }

  tasks_t make_publisher_tasks(auto& context)
  {
    tasks_t on_request_tasks{};
    for (auto&& handle_request : m_on_request_callbacks) {
      on_request_tasks.push_back(make_publisher_task(std::move(handle_request), context));
    };

    return on_request_tasks;
  }

  task_t make_publisher_task(publisher_callback_t&& callback, auto& context)
  {
    static constexpr std::size_t STRIDE_LENGTH = 1;
    auto& status = context.status;
    [[maybe_unused]] auto id = status.get_id();

    status.publishers_are_active().store(true);

    std::size_t last_buffer_sequence = 0;
    const auto handle_message = [&](bool last_message) -> task_t {
      flow::logging::trace("[pub:{}] spin: last_buffer_sequencer {} {}", id, last_buffer_sequence, flow::to_string(status));

      const auto buffer_sequences = co_await context.sequencer.claim_up_to(STRIDE_LENGTH, context.scheduler);

      for (auto const& buffer_sequence : buffer_sequences) {
        last_buffer_sequence = buffer_sequence;
        flow::logging::trace("[pub:{}] buffer_sequence {}", id, buffer_sequence);

        auto& message_wrapper = context.buffer[buffer_sequence & context.index_mask];
        message_wrapper.metadata.sequence = ++std::atomic_ref(m_sequence);
        message_wrapper.metadata.last_message = last_message;

        const auto callback_routine = [&]() -> task_t {
          invoke(callback, message_wrapper);
          co_return;
        };

        co_await callback_routine();
      }

      context.sequencer.publish(buffer_sequences);
    };

    while (not callback.is_cancellation_requested() and status.num_subscribers() > 0) {
      cppcoro::sync_wait(handle_message(false));
    }
    --status.num_publishers();

    status.publishers_are_active().store(false);
    while (status.num_publishers() == 0 and status.num_subscribers() > 0 and (last_buffer_sequence <= (context.barrier.last_published() + config_t::channel::message_buffer_size))) {
      flow::logging::trace("[pub:{}] flush: last_buffer_sequence {} <= context.barrier.last_published(): {}", id, last_buffer_sequence, context.barrier.last_published());
      co_await handle_message(true);
    }

    flow::logging::trace("[pub:{}] done: {}", id, flow::to_string(status));
    co_return;
  }

  tasks_t make_subscriber_tasks(auto& context)
  {
    tasks_t on_message_tasks{};
    for (auto&& handle_message : m_on_message_callbacks) {
      on_message_tasks.push_back(make_subscriber_task(std::move(handle_message), context));
    };

    return on_message_tasks;
  }

  task_t make_subscriber_task(subscriber_callback_t&& callback, auto& context)
  {
    auto& status = context.status;
    [[maybe_unused]] auto id = status.get_id();
    status.subscribers_are_active().store(true);

    flow::spin_wait wait;
    std::atomic_size_t next_to_read = 1;
    std::atomic_size_t last_published = 0;

    const auto handle_message = [&]() -> task_t {
      flow::logging::trace("[sub:{}] spin: {}", id, flow::to_string(status));

      flow::logging::trace("[sub:{}] WAIT: next to read {} last published {} last published after: {}", id, next_to_read, last_published, context.sequencer.last_published_after(last_published));
      const size_t available = co_await context.sequencer.wait_until_published(next_to_read, last_published, context.scheduler);
      flow::logging::trace("[sub:{}] GOT: available: {}", id, available);

      while (flow::atomic_post_increment(next_to_read) < available) {
        const std::size_t current_sequence = next_to_read;
        auto& message_wrapper = context.buffer[current_sequence & context.index_mask];
        const auto callback_routine = [&]() -> task_t {
          invoke(callback, message_wrapper);
          co_return;
        };

        co_await callback_routine();
      }

      context.barrier.publish(available);
      last_published = available;
    };

    while (not callback.is_cancellation_requested() and status.num_publishers() > 0) {
      co_await handle_message();
    }

    --status.num_subscribers();

    while (status.num_publishers() > 0 and status.num_subscribers() == 0 and next_to_read < context.sequencer.last_published_after(last_published)) {
      flow::logging::debug("[sub:{}] flush: available: {}", id, last_published);
      status.subscribers_are_active().store(false);
      co_await handle_message();
    }

    flow::logging::trace("[sub:{}] done: {}", id, flow::to_string(status));
    co_return;
  }

  publisher_callbacks_t m_on_request_callbacks{};
  subscriber_callbacks_t m_on_message_callbacks{};

  std::string m_name;
  std::size_t m_sequence{};
};

}// namespace flow

#endif