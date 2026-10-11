#pragma once

#include "vaecu/demo_scenario.hpp"

#include <string>

namespace vaecu::bridge {
[[nodiscard]] DemoScenario parse_scenario(const std::string& name);
[[nodiscard]] std::string event_json(const DemoEvent& event);
[[nodiscard]] std::string summary_json(const DemoSummary& summary, bool cancelled);
} // namespace vaecu::bridge
