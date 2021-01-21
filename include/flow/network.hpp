#pragma once

#include <cppcoro/on_scope_exit.hpp>
#include <cppcoro/task.hpp>
#include <cppcoro/when_all_ready.hpp>

#include "channel_set.hpp"
#include "flow/cancellable_function.hpp"
#include "flow/channel.hpp"
#include "flow/context.hpp"
#include "flow/routine.hpp"
#include "flow/spin.hpp"
#include "flow/timeout_routine.hpp"

/**
 * A network is a sequence of routines connected by single producer single consumer channels.
 * The end of the network depends on the data flow from the beginning of the network. The beginning
 * of the network has no dependencies.
 *
 * An empty network is a network which has no routines and can take a spinner or a producer.
 *
 * The minimal network s a network with a spinner, because it depends on nothing and nothing depends on it.
 *
 * When a network is begun with a producer, transformers may be inserted into the network until it is capped
 * with a consumer.
 *
 * Each network is considered independent from another network and may not communicate with each other.
 *
 * producer -> transfomer -> ... -> consumer
 *
 * Each channel in the network uses contiguous memory to pass data to the channel waiting on the other way. All
 * data must flow from producer to consumer; no cyclical dependencies.
 *
 * Cancellation
 * Cancelling the network of coroutines is a bit tricky because if you stop them all at once, some of them will hang
 * with no way to have them leave the awaiting state.
 *
 * When starting the network reaction all routines will begin to wait and the first routine that is given priority is the
 * producer at the beginning of the network, and the last will be the end of the network, or consumer.
 *
 * The consumer then has to be the one that initializes the cancellation. The algorithm is as follows:
 *
 * Consumer receives cancellation request from the cancellation handle
 * consumer terminates the channel it is communicating with
 * consumer flushes out any awaiting producers/transformers on the producing end of the channel
 * end routine
 *
 * The transformer or producer that is next in the network will then receiving channel termination notification
 * from the consumer at the end of the network and break out of its loop
 * It well then notify terminate the producer channel it receives data from and flush it out
 *
 * rinse repeat until the beginning of the network, which is a producer
 * The producer simply breaks out of its loop and exits the scope
 */

namespace flow {
template<typename configuration_t>
class network {
public:

  enum class state {
    empty, /// Initial state
    open, /// Has producers and transformers with no consumer
    closed /// The network is complete and is capped by a consumer
  };

  /*
   * @param context The context contains all raw resources used to create the network
   */
  explicit network(auto* context) : m_context(context) {}

  /**
   * Makes a channel if it doesn't exist and returns a reference to it
   * @tparam message_t The message type the channel will communicate
   * @param channel_name The name of the channel (optional)
   * @return A reference to the channel
   */
  template<typename message_t>
  auto& make_channel_if_not_exists(std::string channel_name)
  {
    if (m_context->channels.template contains<message_t>(channel_name)) {
      return m_context->channels.template at<message_t>(channel_name);
    }

    using channel_t = channel<message_t, configuration_t>;

    channel_t channel{
      channel_name,
      get_channel_resource(m_context->resource_generator),
      &m_context->thread_pool
    };

    m_context->channels.put(std::move(channel));
    return m_context->channels.template at<message_t>(channel_name);
  }

  /**
   * Pushes a routine into the network
   * @param spinner A routine with no dependencies and nothing depends on it
   */
  void push(flow::spinner auto&& spinner)
  {
    using spinner_t = decltype(spinner);
    auto cancellable = flow::make_cancellable_function(std::forward<spinner_t>(spinner));

    m_handle = cancellable->handle();
    m_context->tasks.push_back(spin_spinner(m_context->thread_pool, *cancellable));
    m_callbacks.push_back(cancellable);
  }

  /**
   * Pushes a producer into the network
   * @param producer The producer routine
   * @param channel_name The channel name the producer will publish to
   */
  void push(flow::producer auto&& producer, std::string channel_name = "")
  {
    using producer_t = decltype(producer);
    using return_t = std::decay_t<typename flow::metaprogramming::function_traits<producer_t>::return_type>;

    auto& channel = make_channel_if_not_exists<return_t>(channel_name);
    auto cancellable = flow::make_cancellable_function(std::forward<producer_t>(producer));

    m_context->tasks.push_back(spin_producer<return_t>(channel, *cancellable));
    m_callbacks.push_back(cancellable);
  }

  /**
   *  Pushes a transformer into the network and creates any necessary channels it requires
   * @param transformer A routine that depends on another routine and is depended on by a consumer or transformer
   * @param producer_channel_name The channel it depends on
   * @param consumer_channel_name The channel that it will publish to
   */
  void push(flow::transformer auto&& transformer, std::string producer_channel_name = "", std::string consumer_channel_name = "")
  {
    using transformer_t = decltype(transformer);
    using argument_t = std::decay_t<typename flow::metaprogramming::function_traits<transformer_t>::template args<0>::type>;
    using return_t = std::decay_t<typename flow::metaprogramming::function_traits<transformer_t>::return_type>;

    auto& producer_channel = make_channel_if_not_exists<argument_t>(producer_channel_name);
    auto& consumer_channel = make_channel_if_not_exists<argument_t>(consumer_channel_name);
    auto cancellable = flow::make_cancellable_function(std::forward<transformer_t>(transformer));

    m_context->tasks.push_back(spin_transformer<return_t, argument_t>(producer_channel, consumer_channel, *cancellable));
    m_callbacks.push_back(cancellable);
  }

  /**
   * Pushes a consumer into the network
   * @param consumer A routine no other routine depends on and depends on at least a single routine
   * @param channel_name The channel it will consume from
   */
  void push(flow::consumer auto&& consumer, std::string channel_name = "")
  {
    using consumer_t = decltype(consumer);
    using argument_t = std::decay_t<typename flow::metaprogramming::function_traits<consumer_t>::template args<0>::type>;

    auto& channel = make_channel_if_not_exists<argument_t>(channel_name);
    auto cancellable = flow::make_cancellable_function(std::forward<consumer_t>(consumer));

    m_handle = cancellable->handle();
    m_context->tasks.push_back(spin_consumer<argument_t>(channel, *cancellable));
    m_callbacks.push_back(cancellable);
  }

  /**
   * Joins all the routines into a single coroutine
   * @return a coroutine
   */
  cppcoro::task<void> spin()
  {
    co_await cppcoro::when_all_ready(std::move(m_context->tasks));
  }

  /**
   * Makes a handle to this network that will allow whoever holds the handle to cancel
   * the network
   *
   * The cancellation handle will trigger the consumer to cancel and trickel down all the way to the producer
   * @return A cancellation handle
   */
  cancellation_handle handle()
  {
    return m_handle;
  }

  /**
   * Cancel the network after the specified time
   *
   * This does not mean the network will be stopped after this amount of time! It takes a non-deterministic
   * amount of time to fully shut the network down.
   * @param time in milliseconds
   */
  void cancel_after(std::chrono::milliseconds time)
  {
    auto timeout_function_ptr = make_timeout_routine(time, [&] {
      handle().request_cancellation();
    });

    auto& timeout_function = *timeout_function_ptr;

    m_context->tasks.push_back(timeout_function());

    // Prioritize timeout task
//    std::ranges::reverse(m_context->tasks);

    m_callbacks.push_back(std::move(timeout_function_ptr));
  }

private:
  context<configuration_t>* m_context;
  std::vector<std::any> m_callbacks;
  cancellation_handle m_handle;
};
}// namespace flow