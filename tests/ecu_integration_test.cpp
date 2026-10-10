#include "vaecu/brake_ecu.hpp"
#include "vaecu/dashboard_ecu.hpp"
#include "vaecu/engine_ecu.hpp"
#include "vaecu/virtual_can_bus.hpp"

#include <gtest/gtest.h>

#include <chrono>

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

TEST(EcuIntegrationTest, RoutesEngineAndBrakeStatusAndProcessesSimulationEvents)
{
    VirtualCanBus bus;
    const auto brake_inbox = bus.register_receiver("brake", {msg::brake_event_id});
    const auto display_inbox = bus.register_receiver("dashboard",
        {msg::engine_telemetry_id, msg::brake_status_id});
    EngineEcu engine{bus, {3000, 88}};
    BrakeEcu brake{bus};
    DashboardEcu dashboard;
    const Timestamp now{10ms};
    engine.start(now);
    brake.start(now);
    dashboard.start(now);
    const auto sent = bus.send(msg::encode_brake_event(msg::BrakeEvent::Apply, now));
    EXPECT_EQ(sent.delivered, 1U);
    EXPECT_EQ(display_inbox->pending(), 0U);
    dispatch(*brake_inbox, brake, now);
    engine.on_tick(now);
    brake.on_tick(now);
    dispatch(*display_inbox, dashboard, now);
    const auto state = dashboard.state();
    ASSERT_TRUE(state.engine);
    ASSERT_TRUE(state.brake);
    EXPECT_EQ(state.engine->engine_rpm, 3000);
    EXPECT_EQ(state.engine->vehicle_speed_kph, 88);
    EXPECT_TRUE(state.brake->applied);
    EXPECT_EQ(state.engine_health, SignalHealth::Fresh);
    EXPECT_EQ(state.brake_health, SignalHealth::Fresh);
    EXPECT_EQ(brake_inbox->pending(), 0U);
    dashboard.stop();
    brake.stop();
    engine.stop();
    EXPECT_EQ(bus.close(), 0U);
}

TEST(EcuIntegrationTest, MissingPublicationsBecomeStaleAndRecoverOnNewTraffic)
{
    VirtualCanBus bus;
    const auto inbox = bus.register_receiver("dashboard",
        {msg::engine_telemetry_id, msg::brake_status_id});
    EngineEcu engine{bus, {1000, 10}};
    BrakeEcu brake{bus};
    DashboardEcu dashboard{{100ms, 100ms}};
    engine.start(Timestamp{0ms});
    brake.start(Timestamp{0ms});
    dashboard.start(Timestamp{0ms});
    engine.on_tick(Timestamp{0ms});
    brake.on_tick(Timestamp{0ms});
    dispatch(*inbox, dashboard, Timestamp{0ms});
    dashboard.on_tick(Timestamp{100ms});
    EXPECT_EQ(dashboard.state().engine_health, SignalHealth::Stale);
    EXPECT_EQ(dashboard.state().brake_health, SignalHealth::Stale);
    engine.set_inputs({2000, 20});
    engine.on_tick(Timestamp{110ms});
    dispatch(*inbox, dashboard, Timestamp{110ms});
    EXPECT_EQ(dashboard.state().engine_health, SignalHealth::Fresh);
    EXPECT_EQ(dashboard.state().brake_health, SignalHealth::Stale);
    EXPECT_EQ(dashboard.state().engine->engine_rpm, 2000);
    dashboard.stop();
    brake.stop();
    engine.stop();
}

TEST(EcuIntegrationTest, OverflowDoesNotSilentlyInventNewDashboardData)
{
    VirtualCanBus bus;
    const auto inbox = bus.register_receiver("dashboard", {msg::engine_telemetry_id}, 1);
    EngineEcu engine{bus, {1000, 10}};
    DashboardEcu dashboard{{100ms, 100ms}};
    engine.start(Timestamp{0ms});
    dashboard.start(Timestamp{0ms});
    engine.on_tick(Timestamp{0ms});
    engine.set_inputs({2000, 20});
    engine.on_tick(Timestamp{100ms});
    ASSERT_TRUE(engine.last_send_result());
    EXPECT_EQ(engine.last_send_result()->dropped, 1U);
    dispatch(*inbox, dashboard, Timestamp{100ms});
    EXPECT_EQ(dashboard.state().engine->engine_rpm, 1000);
    EXPECT_EQ(dashboard.state().engine_health, SignalHealth::Stale);
    engine.on_tick(Timestamp{110ms});
    dispatch(*inbox, dashboard, Timestamp{110ms});
    EXPECT_EQ(dashboard.state().engine->engine_rpm, 2000);
    EXPECT_EQ(dashboard.state().engine_health, SignalHealth::Fresh);
    dashboard.stop();
    engine.stop();
}

} // namespace
