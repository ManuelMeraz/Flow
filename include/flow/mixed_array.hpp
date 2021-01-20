#ifndef FLOW_MIXED_ARRAY_HPP
#define FLOW_MIXED_ARRAY_HPP

#include <algorithm>
#include <array>
#include <tuple>
#include <variant>

#include "flow/logging.hpp"
#include "flow/metaprogramming.hpp"

namespace flow {

template<std::size_t N, typename... set_of_types_t>
class mixed_array {
  using mixed_t = std::variant<set_of_types_t...>;
  using array_t = std::array<mixed_t, N>;

public:
  template<typename... Types>
  constexpr explicit mixed_array(Types&&... ts) : m_items{ { std::forward<Types>(ts)... } } {}

  using iterator = typename array_t::iterator;
  using const_iterator = typename array_t::const_iterator;
  constexpr mixed_array() = default;
  constexpr mixed_array(mixed_array const& other)
  {
    std::copy(std::begin(other), std::end(other), std::begin(*this));
  }

  constexpr mixed_array (mixed_array && other) noexcept
  {
    for (std::size_t i = 0; i < N; ++i) {
      m_items[i] = std::move(other.m_items[i]);
    }
  }

  constexpr mixed_array& operator=(mixed_array const& other)
  {
    std::copy(std::begin(other), std::end(other), std::begin(*this));
    return *this;
  }

  constexpr mixed_array& operator=(mixed_array && other) noexcept
  {
    for (std::size_t i = 0; i < N; ++i) {
      m_items[i] = std::move(other.m_items[i]);
    }
    return *this;
  }
  constexpr decltype(auto) begin() { return std::begin(m_items); }
  constexpr decltype(auto) begin() const { return std::begin(m_items); }
  constexpr decltype(auto) end() { return std::end(m_items); }
  constexpr decltype(auto) end() const { return std::end(m_items); }

  [[maybe_unused]] constexpr mixed_t& front() const { return m_items.front(); }
  [[maybe_unused]] constexpr mixed_t& back() const { return m_items.back(); }

  constexpr mixed_t& operator[](std::size_t index) { return m_items[index]; }
  [[maybe_unused]] constexpr std::size_t size() const { return m_items.size(); }

private:
  array_t m_items;
};

template<typename... types_t>
constexpr auto make_mixed_array(types_t&&... types)
{
  auto type_set = metaprogramming::make_type_set<types_t...>();

  const auto to_mixed_array = [&]<typename... TypeSet>(std::tuple<TypeSet...> /*unused*/)
  {
    return mixed_array<sizeof...(types), TypeSet...>(types...);
  };

  return to_mixed_array(type_set);
}

template<class... Ts>
struct overloaded : Ts... {
  using Ts::operator()...;
};
template<class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

template<typename... Visitors>
constexpr auto make_visitor(Visitors&&... visitors)
{
  return [&](auto& item) {
    return std::visit(overloaded{ std::forward<decltype(visitors)>(visitors)... }, item);
  };
}

}// namespace flow

#endif//FLOW_MIXED_ARRAY_HPP