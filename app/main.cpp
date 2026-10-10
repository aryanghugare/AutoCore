#include "vaecu/brake_ecu.hpp"
#include "vaecu/dashboard_ecu.hpp"
#include "vaecu/engine_ecu.hpp"
#include "vaecu/virtual_can_bus.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>

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

} // namespace

int main()
{
    VirtualCanBus bus;
    const auto brake_inbox = bus.register_receiver("brake", {msg::brake_event_id});
    const auto display_inbox = bus.register_receiver("dashboard",
        {msg::engine_telemetry_id, msg::brake_status_id});
    EngineEcu engine{bus, {1500, 60}};
    BrakeEcu brake{bus};
    DashboardEcu dashboard;
    engine.start(Timestamp{0ms});
    brake.start(Timestamp{0ms});
    dashboard.start(Timestamp{0ms});

    const auto publish = [&](Timestamp now) {
        engine.on_tick(now);
        brake.on_tick(now);
        dispatch(*display_inbox, dashboard, now);
    };
    publish(Timestamp{0ms});
    print_state("normal", dashboard.state());

    const auto event = bus.send(msg::encode_brake_event(msg::BrakeEvent::Apply, Timestamp{100ms}));
    if (event.status != SendStatus::Routed || event.delivered != 1 || event.dropped != 0) {
        throw std::runtime_error("Demo brake event was not delivered");
    }
    dispatch(*brake_inbox, brake, Timestamp{100ms});
    engine.set_inputs({2500, 80});
    publish(Timestamp{100ms});
    print_state("apply", dashboard.state());

    dashboard.on_tick(Timestamp{600ms});
    print_state("stale", dashboard.state());
    publish(Timestamp{700ms});
    print_state("recovery", dashboard.state());

    dashboard.stop();
    brake.stop();
    engine.stop();
    (void)bus.close();
    return 0;
}
