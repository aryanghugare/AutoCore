#pragma once

#include "vaecu/can_transport.hpp"
#include "vaecu/dashboard_ecu.hpp"
#include "vaecu/diagnostics.hpp"

#include <chrono>
#include <cstddef>
#include <functional>
#include <variant>
#include <vector>

namespace vaecu {

enum class DemoScenario { Normal, SensorFailure, CommunicationTimeout, Recovery };
enum class DemoActionKind { Started, SensorFailed, EnginePaused, SensorRecovered, EngineResumed, Stopped };
enum class DemoSource { Engine, Brake, Scenario };
enum class EcuAvailability { Online, Offline };

struct DemoAction {
    Timestamp timestamp;
    DemoActionKind kind;
};

struct DemoTransmission {
    DemoSource source;
    CanFrame frame;
    SendResult result; // Admission counts, not callback-completion counts.
};

struct DemoStatus {
    Timestamp timestamp;
    EcuAvailability engine;
    EcuAvailability brake;
    EcuAvailability dashboard;
    DashboardState display;
};

using DemoEvent = std::variant<DemoAction, DemoTransmission, DiagnosticEvent, DemoStatus>;

struct DemoSummary {
    std::chrono::milliseconds duration{0};
    std::size_t transmissions{0};
    std::size_t admitted_copies{0};
    std::size_t processed_copies{0};
    std::size_t dropped_copies{0};
    std::size_t faults_raised{0};
    std::size_t faults_recovered{0};
    std::size_t active_faults{0};
    std::size_t discarded_events{0};
    std::size_t pending_discarded{0};
};

struct DemoReport {
    DemoScenario scenario;
    std::vector<DemoEvent> events; // Owned, bounded by the fixed scenario schedule.
    DemoSummary summary;
};

// Optional host callbacks, invoked synchronously on the scenario's caller.
// await_cycle receives the next relative cycle time; false cancels before that
// cycle. observe receives each owned trace event after insertion. Callbacks
// must not reenter this session and must copy events retained beyond the call.
// Exceptions propagate after RAII cleanup.
// Without hooks, execution remains deterministic and has no waits.
struct DemoHooks {
    std::function<bool(std::chrono::milliseconds)> await_cycle;
    std::function<void(const DemoEvent&)> observe;
};

// Runs real ECU/bus/diagnostic behavior with injected time. No output, sleeps,
// or threads. Four fixed plans, at most seven 250 ms cycles. Invalid enum values
// throw invalid_argument. Cleanup occurs on success and exceptions; each call
// owns a fresh session. Shutdown does not manufacture diagnostic recovery.
[[nodiscard]] DemoReport run_demo_scenario(DemoScenario scenario, DemoHooks hooks = {});

} // namespace vaecu
