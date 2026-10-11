#include "vaecu/brake_ecu.hpp"
#include "vaecu/concurrent_simulation.hpp"
#include "vaecu/dashboard_ecu.hpp"
#include "vaecu/diagnostics.hpp"
#include "vaecu/engine_ecu.hpp"
#include "vaecu/virtual_can_bus.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
using namespace vaecu;
using namespace std::chrono_literals;
namespace msg = vaecu::messages;

void dispatch(ICanReceiver& inbox, IEcu& ecu, Timestamp now)
{
    for (;;) {
        const auto result = inbox.try_receive();
        const auto* frame = std::get_if<CanFrame>(&result);
        if (!frame) return;
        ecu.on_message(*frame, now);
    }
}

const char* health_label(SignalHealth health) noexcept
{
    switch (health) {
    case SignalHealth::Missing: return "missing";
    case SignalHealth::Fresh: return "fresh";
    case SignalHealth::Stale: return "stale";
    }
    return "unknown";
}

void print_state(const char* scenario, DashboardState state)
{
    std::cout << scenario << ": engine=" << health_label(state.engine_health);
    if (state.engine) {
        std::cout << " rpm=" << state.engine->engine_rpm
                  << " speed_kph=" << state.engine->vehicle_speed_kph;
    }
    std::cout << " brake=" << health_label(state.brake_health);
    if (state.brake) std::cout << " applied=" << (state.brake->applied ? "true" : "false");
    std::cout << '\n';
}

// Presentation consumes event values; detection and storage do not print.
void print_diagnostics(DiagnosticStore& store)
{
    while (const auto event = store.take_event()) {
        std::cout << "diagnostic: code=0x" << std::hex << static_cast<unsigned>(event->id)
                  << std::dec << ' '
                  << (event->kind == DiagnosticEventKind::Raised ? "active" : "recovered")
                  << " time_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(
                         event->timestamp.time_since_epoch()).count() << '\n';
    }
}

} // namespace

int deterministic_demo()
{
    VirtualCanBus bus;
    DiagnosticStore diagnostic_store;
    DiagnosticMonitor diagnostics{diagnostic_store};
    const auto brake_inbox = bus.register_receiver("brake", {msg::brake_event_id});
    const auto display_inbox = bus.register_receiver("dashboard",
        {msg::engine_telemetry_id, msg::brake_status_id});
    const auto diagnostic_inbox = bus.register_receiver("diagnostics",
        {msg::engine_telemetry_id, msg::brake_status_id, msg::brake_event_id});
    EngineEcu engine{bus, {1500, 60}};
    BrakeEcu brake{bus};
    DashboardEcu dashboard;
    engine.start(Timestamp{0ms});
    brake.start(Timestamp{0ms});
    dashboard.start(Timestamp{0ms});
    diagnostics.start(Timestamp{0ms});
    diagnostics.observe_engine_inputs(1500, 60, Timestamp{0ms});

    const auto publish = [&](Timestamp now) {
        engine.on_tick(now);
        brake.on_tick(now);
        dispatch(*display_inbox, dashboard, now);
        dispatch(*diagnostic_inbox, diagnostics, now);
        diagnostics.on_tick(now);
    };
    publish(Timestamp{0ms});
    print_state("normal", dashboard.state());

    const auto event = bus.send(msg::encode_brake_event(msg::BrakeEvent::Apply, Timestamp{100ms}));
    if (event.status != SendStatus::Routed || event.delivered != 2 || event.dropped != 0) {
        throw std::runtime_error("Demo brake event was not delivered");
    }
    dispatch(*brake_inbox, brake, Timestamp{100ms});
    diagnostics.observe_engine_inputs(2500, 80, Timestamp{100ms});
    engine.set_inputs({2500, 80});
    publish(Timestamp{100ms});
    print_state("apply", dashboard.state());

    // Observe a bad raw sample without replacing the ECU's last valid inputs.
    diagnostics.observe_engine_inputs(-1, 80, Timestamp{200ms});
    print_diagnostics(diagnostic_store);
    diagnostics.observe_engine_inputs(2500, 80, Timestamp{300ms});
    print_diagnostics(diagnostic_store);

    // Valid CAN-inspired envelope, invalid application-level engine reading.
    const auto malformed = bus.send(CanFrame{msg::engine_telemetry_id,
        {255, 255}, 4, Timestamp{400ms}});
    if (malformed.status != SendStatus::Routed || malformed.delivered != 2 || malformed.dropped != 0) {
        throw std::runtime_error("Demo malformed payload was not delivered");
    }
    dispatch(*display_inbox, dashboard, Timestamp{400ms});
    dispatch(*diagnostic_inbox, diagnostics, Timestamp{400ms});
    print_diagnostics(diagnostic_store);

    dashboard.on_tick(Timestamp{600ms});
    diagnostics.on_tick(Timestamp{600ms});
    print_state("stale", dashboard.state());
    print_diagnostics(diagnostic_store);
    diagnostics.on_tick(Timestamp{1400ms});
    print_diagnostics(diagnostic_store);
    publish(Timestamp{1500ms});
    print_state("recovery", dashboard.state());
    print_diagnostics(diagnostic_store);

    dashboard.stop();
    brake.stop();
    engine.stop();
    diagnostics.stop();
    (void)bus.close();
    return 0;
}

int concurrent_demo()
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
    print_state("concurrent normal", await_state([](const DashboardState& state) {
        return state.engine_health == SignalHealth::Fresh && state.brake_health == SignalHealth::Fresh;
    }));
    const auto sent = simulation.send(msg::encode_brake_event(msg::BrakeEvent::Apply,
        Timestamp::clock::now()));
    if (sent.delivered != 2 || sent.dropped != 0) {
        throw std::runtime_error("Concurrent brake event admission failed");
    }
    print_state("concurrent apply", await_state([](const DashboardState& state) {
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
    print_state("concurrent timeout", simulation.dashboard_snapshot().state);
    simulation.set_engine_publishing(true);
    await_timeout_event(DiagnosticEventKind::Recovered);
    print_state("concurrent recovery", await_state([](const DashboardState& state) {
        return state.engine_health == SignalHealth::Fresh;
    }));
    simulation.stop();
    simulation.rethrow_if_failed();
    std::cout << "concurrent shutdown: workers joined\n";
    return 0;
}

int main(int argc, char* argv[])
{
    try {
        if (argc == 1) return deterministic_demo();
        if (argc == 2 && std::string_view{argv[1]} == "--concurrent") return concurrent_demo();
        std::cerr << "Usage: vaecu_sim [--concurrent]\n";
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "Simulation failed: " << error.what() << '\n';
        return 1;
    }
}
