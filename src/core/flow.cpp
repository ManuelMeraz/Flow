#include <iostream>
#include <docopt/docopt.h>

namespace flow {
void begin(int argc, const char ** argv)
{
  static constexpr auto USAGE =
    R"(Flow

     Usage:
          app

     Options:
              -h --help     Show this screen.
              --version     Show version.
    )";

  static auto constexpr SHOW_HELP_IF_REQUESTED = true;
  static auto constexpr VERSION_INFO = "Flow 0.1";

  auto const argument_values = std::vector<std::string>{ std::next(argv), std::next(argv, argc) };
  auto const arguments = docopt::docopt(USAGE, argument_values, SHOW_HELP_IF_REQUESTED, VERSION_INFO);

  for (auto const&[argument, value] : arguments) {
    std::cout << "{" << argument << ", " << value << "}";
  }
}
}// namespace src

