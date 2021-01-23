#pragma once

#include <stack>

#include "channel_resource.hpp"
#include "consumer_token.hpp"
#include "producer_token.hpp"

#include <cppcoro/async_generator.hpp>
#include <cppcoro/multi_producer_sequencer.hpp>
#include <cppcoro/static_thread_pool.hpp>
#include <cppcoro/task.hpp>

/**
 * The link between routines in a network are m_channels.
 *
 * A multi multi_channel in this framework is a multi producer multi consumer multi_channel that are linked
 * to two corresponding neighbors in a network.
 */

namespace flow::detail {

/**
 * A multi producer multi consumer channel
 *
 * This means that because the assumption is that multiple producers and consumers will be used to
 * communicate through this single channel, there will be a performance cost of atomics for synchronization
 *
 * @tparam raw_message_t The raw message type is the message type with references potentially attached
 * @tparam configuration_t The global compile time configuration
 */
template<typename raw_message_t, typename configuration_t>
class multi_channel {
  using message_t = std::decay_t<raw_message_t>;/// Remove references
  using resource_t = channel_resource<configuration_t, cppcoro::multi_producer_sequencer<std::size_t>>;
  using scheduler_t = cppcoro::static_thread_pool;/// The static thread pool is used to schedule threads

public:
  /**
   * @param name Name of the multi_channel
   * @param resource A generated multi_channel channel_resource
   * @param scheduler The global scheduler
   */
  multi_channel(std::string name, resource_t* resource, scheduler_t* scheduler)
    : m_name{ std::move(name) },
      m_resource{ resource },
      m_scheduler{ scheduler }
  {
  }

  multi_channel& operator=(multi_channel const& other)
  {
    m_resource = other.m_resource;
    m_scheduler = other.m_scheduler;
    std::copy(std::begin(other.buffer), std::end(other.buffer), std::begin(m_buffer));
    return *this;
  }

  multi_channel(multi_channel const& other)
  {
    m_resource = other.m_resource;
    m_scheduler = other.m_scheduler;
    std::copy(std::begin(other.m_buffer), std::end(other.m_buffer), std::begin(m_buffer));
  }

  multi_channel& operator=(multi_channel&& other) noexcept
  {
    m_resource = other.m_resource;
    m_scheduler = other.m_scheduler;
    std::move(std::begin(other.m_buffer), std::end(other.m_buffer), std::begin(m_buffer));
    return *this;
  }

  multi_channel(multi_channel&& other) noexcept : m_resource(other.m_resource)
  {
    *this = std::move(other);
  }

  /**
   * Used to store the multi_channel into a multi_channel set
   * @return The hash of the message and multi_channel name
   */
  std::size_t hash() { return typeid(message_t).hash_code() ^ std::hash<std::string>{}(m_name); }

  /*******************************************************
   ****************** PRODUCER INTERFACE *****************
   ******************************************************/

  /**
   * Request permission to publish the next message
   * @return
   */
  cppcoro::task<void> request_permission_to_publish(producer_token<message_t>& token)
  {
    static constexpr std::size_t STRIDE_LENGTH = configuration_t::stride_length;

    ++std::atomic_ref(m_num_publishers_waiting);
    cppcoro::sequence_range<std::size_t> sequences = co_await m_resource->sequencer.claim_up_to(STRIDE_LENGTH, *m_scheduler);
    --std::atomic_ref(m_num_publishers_waiting);

    token.sequences = std::move(sequences);
  }

  /**
   * Publish the produced message
   * @param message The message type the multi_channel communicates
   */
  void publish_messages(producer_token<message_t>& token)
  {
    for (auto& sequence_number : token.sequences) {
      m_buffer[sequence_number & m_index_mask] = token.messages.top();
      token.messages.pop();
    }

    m_resource->sequencer.publish(std::move(token.sequences));
  }

  /*******************************************************
   ****************** CONSUMER INTERFACE *****************
   ******************************************************/

  /**
   * Retrieve an iterable message generator. Will generate all messages that
   * have already been published by a callable_producer
   * @return a message generator
   */
  cppcoro::async_generator<message_t> message_generator(consumer_token<message_t>& token)
  {
    token.end_sequence = co_await m_resource->sequencer.wait_until_published(
      token.sequence, token.last_sequence_published, *m_scheduler);

    do {
      co_yield m_buffer[token.sequence & m_index_mask];
    } while (std::atomic_ref(token.sequence)++ < token.end_sequence);
  }


  /**
   * Notify the callable_producer to produce the next messages
   */
  void notify_message_consumed(consumer_token<message_t>& token)
  {
    if (token.sequence == token.end_sequence) {
      m_resource->barrier.publish(token.end_sequence);
      token.last_sequence_published = token.end_sequence;
    }
  }

  /**
   * Disable the multi_channel
   *
   * When cancelling a network the beginning of the cancellation happens
   * with the callable_consumer end of the network. This trickles down all the way to the
   * beginning end of the network with the first callable_producer.
   *
   * The callable_consumer cancels itself, terminates the multi_channel, and then flushes out any
   * producers waiting for permission to publish.
   */
  void terminate()
  {
    std::atomic_ref(m_is_terminated).store(true);
  }

  /**
   * Used by the callable_producer ends of the m_channels to keep looping or not
   * @return if the multi_channel has been cancelled by the callable_consumer end of the multi_channel
   */
  bool is_terminated()
  {
    return std::atomic_ref(m_is_terminated).load();
  }

  /**
   * @return if any callable_producer m_channels are currently waiting for permission
   */
  bool is_waiting()
  {
    return std::atomic_ref(m_num_publishers_waiting).load() > 0;
  }

private:
  bool m_is_terminated{};

  std::size_t m_num_publishers_waiting{};

  /// The message buffer size determines how many messages can communicated at once
  std::array<message_t, configuration_t::message_buffer_size> m_buffer{};
  std::size_t m_index_mask = configuration_t::message_buffer_size - 1;

  std::string m_name;

  /// Non owning
  resource_t* m_resource{ nullptr };
  scheduler_t* m_scheduler{ nullptr };
};
}// namespace flow::detail