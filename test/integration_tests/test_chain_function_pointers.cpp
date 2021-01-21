#include <chrono>

#include <flow/configuration.hpp>
#include <flow/consumer.hpp>
#include <flow/logging.hpp>
#include <flow/network.hpp>
#include <flow/producer.hpp>
#include <flow/transformer.hpp>
#include <flow/flow.hpp>

#include <cppcoro/sync_wait.hpp>

using namespace std::literals;

int producer()
{
  //  flow::logging::error("callable_producer");
  static int val = 0;
  return val++;
}

int transformer(int&& val)
{
  //  flow::logging::error("callable_transformer");
  return val * 2;
}

void consumer(int&& val)
{
  flow::logging::error("callable_consumer: {}", val);
}

int main()
{
  using network_t  = flow::network<flow::configuration>;

  network_t network = flow::make_network(
    flow::make_producer(producer, "producer"),
    flow::make_transformer(transformer, "producer", "consumer"),
    flow::make_consumer(consumer, "consumer"));

  network.cancel_after(0s);
  flow::spin(std::move(network));
}
