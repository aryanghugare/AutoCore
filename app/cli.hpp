#pragma once

#include "vaecu/demo_scenario.hpp"

#include <iosfwd>
#include <string_view>
#include <vector>

namespace vaecu::cli {

enum class Mode { Scenario, Help, Concurrent };
struct Options {
    Mode mode{Mode::Scenario};
    DemoScenario scenario{DemoScenario::Normal};
    bool summary_only{false};
};

// Arguments exclude argv[0]. Invalid/duplicate/conflicting options throw
// invalid_argument. Parsing does not construct or start simulation components.
[[nodiscard]] Options parse_arguments(const std::vector<std::string_view>& arguments);
void print_report(const DemoReport& report, bool summary_only, std::ostream& output);
// 0: success/help, 2: usage error, 1: simulation or output failure.
int run(const std::vector<std::string_view>& arguments, std::ostream& output, std::ostream& error);

} // namespace vaecu::cli
