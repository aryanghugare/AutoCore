#pragma once

#include <chrono>

namespace vaecu {

// Monotonic, process-local simulation time; not UTC or a CAN wire field.
using Timestamp = std::chrono::steady_clock::time_point;

} // namespace vaecu
