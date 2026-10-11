#include "vaecu/concurrent_simulation.hpp"

#include <ostream>
#include <stdexcept>

namespace vaecu::cli {
namespace {
using namespace std::chrono_literals;
namespace msg = vaecu::messages;

const char* health_label(SignalHealth health) noexcept
{
    switch (health) {
    case SignalHealth::Missing: return "missing";
    case SignalHealth::Fresh: return "fresh";
    case SignalHealth::Stale: return "stale";
    }
    return "unknown";
}

void print_state(std::ostream& output, const char* scenario, DashboardState state)
{
    output << scenario << ": engine=" << health_label(state.engine_health);
    if (state.engine) {
        output << " rpm=" << state.engine->engine_rpm
                  << " speed_kph=" << state.engine->vehicle_speed_kph;
    }
    output << " brake=" << health_label(state.brake_health);
    if (state.brake) output << " applied=" << (state.brake->applied ? "true" : "false");
    output << '\n';
}

} // namespace

int run_concurrent_demo(std::ostream& output)
{
    ConcurrentSimulation simulation;
    simulation.start();
    const auto await_state = [&](const auto& predicate) {
        auto snapshot = simulation.dashboard_snapshot();
        const auto deadline = Timestamp::clock::now() + 3s;
        while (!predicate(snapshot.state)) {
            const auto next = simulation.wait_for_dashboard(snapshot.revision, deadline);
            if (!next) {
                simulation.rethrow_if_failed();
                throw std::runtime_error("Concurrent demo telemetry deadline elapsed");
            }
            snapshot = *next;
        }
        return snapshot.state;
    };
    print_state(output, "concurrent normal", await_state([](const DashboardState& state) {
        return state.engine_health == SignalHealth::Fresh && state.brake_health == SignalHealth::Fresh;
    }));
    const auto sent = simulation.send(msg::encode_brake_event(msg::BrakeEvent::Apply,
        Timestamp::clock::now()));
    if (sent.delivered != 2 || sent.dropped != 0) {
        throw std::runtime_error("Concurrent brake event admission failed");
    }
    print_state(output, "concurrent apply", await_state([](const DashboardState& state) {
        return state.brake && state.brake->applied;
    }));
    const auto await_timeout_event = [&](DiagnosticEventKind kind) {
        const auto deadline = Timestamp::clock::now() + 3s;
        for (;;) {
            const auto event = simulation.wait_event_until(deadline);
            if (!event) {
                simulation.rethrow_if_failed();
                throw std::runtime_error("Concurrent demo diagnostic deadline elapsed");
            }
            if (event->id == FaultId::EngineCommunicationTimeout && event->kind == kind) return;
        }
    };
    simulation.set_engine_publishing(false);
    await_timeout_event(DiagnosticEventKind::Raised);
    print_state(output, "concurrent timeout", simulation.dashboard_snapshot().state);
    simulation.set_engine_publishing(true);
    await_timeout_event(DiagnosticEventKind::Recovered);
    print_state(output, "concurrent recovery", await_state([](const DashboardState& state) {
        return state.engine_health == SignalHealth::Fresh;
    }));
    simulation.stop();
    simulation.rethrow_if_failed();
    output << "concurrent shutdown: workers joined\n";
    if (!output) throw std::runtime_error("Unable to write concurrent demonstration output");
    return 0;
}

} // namespace vaecu::cli
