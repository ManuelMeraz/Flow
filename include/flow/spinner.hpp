#pragma once

namespace flow {
class spinner {
public:
  using is_spinner = std::true_type;
  using is_routine = std::true_type;

  spinner() = default;
  ~spinner() = default;

  spinner(spinner&&) noexcept = default;
  spinner(spinner const&) = default;
  spinner& operator=(spinner&&) noexcept = default;
  spinner& operator=(spinner const&) = default;

  spinner(flow::spinner_function auto&& callback)
    : m_callback(detail::make_shared_cancellable_function(std::forward<decltype(callback)>(callback))) {}

  auto& callback() { return *m_callback; }

private:
  detail::cancellable_function<void()>::sPtr m_callback{ nullptr };
};

auto make_spinner(std::function<void()>&& callback)
{
  using callback_t = decltype(callback);
  return spinner{std::forward<callback_t>(callback)};
}

auto make_spinner(void (*callback)())
{
  using callback_t = decltype(callback);
  return spinner{std::forward<callback_t>(callback)};
}

auto make_spinner(auto&& lambda)
{
  using callback_t = decltype(lambda);
  return make_spinner(detail::metaprogramming::to_function(std::forward<callback_t>(lambda)));
}


template<typename spinner_t>
concept spinner_routine = std::is_same_v<typename spinner_t::is_spinner, std::true_type>;
}// namespace flow