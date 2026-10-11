#include "vaecu/brake_ecu.hpp"
#include "vaecu/dashboard_ecu.hpp"
#include "vaecu/diagnostics.hpp"
#include "vaecu/engine_ecu.hpp"
#include "vaecu/virtual_can_bus.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <stdexcept>

namespace {
using namespace vaecu;
using namespace std::chrono_literals;
namespace msg = vaecu::messages;

void dispatch(ICanReceiver& inbox, IEcu& consumer, Timestamp now)
{
    for (;;) {
        const auto result = inbox.try_receive();
        const auto* frame = std::get_if<CanFrame>(&result);
        if (!frame) return;
        consumer.on_message(*frame, now);
    }
}

TEST(DiagnosticIntegrationTest, ExistingEcusRouteThroughIndependentDiagnosticAndDashboardInboxes)
{
    VirtualCanBus bus;
    DiagnosticStore store;
    DiagnosticMonitor monitor{store};
    const auto diag = bus.register_receiver("diagnostics",
        {msg::engine_telemetry_id, msg::brake_status_id, msg::brake_event_id});
    const auto display = bus.register_receiver("dashboard",
        {msg::engine_telemetry_id, msg::brake_status_id});
    EngineEcu engine{bus, {2000, 80}};
    BrakeEcu brake{bus};
    DashboardEcu dashboard;
    monitor.start(Timestamp{0ms});
    engine.start(Timestamp{0ms});
    brake.start(Timestamp{0ms});
    dashboard.start(Timestamp{0ms});
    engine.on_tick(Timestamp{0ms});
    brake.on_tick(Timestamp{0ms});
    dispatch(*display, dashboard, Timestamp{0ms});
    dispatch(*diag, monitor, Timestamp{0ms});
    EXPECT_EQ(store.pending_events(), 0U);
    EXPECT_EQ(dashboard.state().engine_health, SignalHealth::Fresh);
    monitor.on_tick(Timestamp{500ms});
    monitor.on_tick(Timestamp{1000ms});
    monitor.on_tick(Timestamp{1001ms});
    EXPECT_EQ(store.pending_events(), 4U);
    engine.on_tick(Timestamp{1200ms});
    dispatch(*diag, monitor, Timestamp{1200ms});
    const auto engine_timeout = store.record(FaultId::EngineCommunicationTimeout);
    const auto brake_timeout = store.record(FaultId::BrakeCommunicationTimeout);
    ASSERT_TRUE(engine_timeout);
    ASSERT_TRUE(brake_timeout);
    EXPECT_EQ(engine_timeout->status, FaultStatus::Cleared);
    EXPECT_EQ(brake_timeout->status, FaultStatus::Active);
    brake.on_tick(Timestamp{1200ms});
    dispatch(*diag, monitor, Timestamp{1200ms});
    dispatch(*display, dashboard, Timestamp{1200ms});
    for (const auto id : {FaultId::EngineMessageStale, FaultId::BrakeMessageStale,
                         FaultId::EngineCommunicationTimeout, FaultId::BrakeCommunicationTimeout}) {
        const auto record = store.record(id);
        ASSERT_TRUE(record);
        EXPECT_EQ(record->status, FaultStatus::Cleared);
        EXPECT_EQ(record->last_recovery, Timestamp{1200ms});
        EXPECT_EQ(record->occurrence_count, 1U);
    }
    EXPECT_EQ(store.pending_events(), 8U);
    EXPECT_EQ(dashboard.state().brake_health, SignalHealth::Fresh);
    dashboard.stop();
    brake.stop();
    engine.stop();
    monitor.stop();
    EXPECT_EQ(bus.close(), 0U);
}

TEST(DiagnosticIntegrationTest, InvalidRawSensorSampleIsRecordedWithoutCorruptingEngineInputs)
{
    VirtualCanBus bus;
    DiagnosticStore store;
    DiagnosticMonitor monitor{store};
    const auto diag = bus.register_receiver("diagnostics", {msg::engine_telemetry_id});
    EngineEcu engine{bus, {1500, 60}};
    monitor.start(Timestamp{0ms});
    engine.start(Timestamp{0ms});
    monitor.observe_engine_inputs(9001, 60, Timestamp{0ms});
    EXPECT_THROW(engine.set_inputs({9001, 60}), std::invalid_argument);
    engine.on_tick(Timestamp{1ms});
    dispatch(*diag, monitor, Timestamp{1ms});
    const auto active = store.record(FaultId::EngineSpeedSensorInvalid);
    ASSERT_TRUE(active);
    EXPECT_EQ(active->status, FaultStatus::Active);
    EXPECT_EQ(engine.inputs().engine_rpm, 1500);
    EXPECT_FALSE(store.record(FaultId::EnginePayloadInvalid));
    monitor.observe_engine_inputs(2000, 80, Timestamp{2ms});
    engine.set_inputs({2000, 80});
    const auto cleared = store.record(FaultId::EngineSpeedSensorInvalid);
    ASSERT_TRUE(cleared);
    EXPECT_EQ(cleared->status, FaultStatus::Cleared);
    EXPECT_EQ(cleared->last_recovery, Timestamp{2ms});
    engine.stop();
    monitor.stop();
}

TEST(DiagnosticIntegrationTest, MalformedApplicationPayloadIsDetectedAfterBusEnvelopeValidation)
{
    VirtualCanBus bus;
    DiagnosticStore store;
    DiagnosticMonitor monitor{store};
    const auto diag = bus.register_receiver("diagnostics", {msg::engine_telemetry_id});
    monitor.start(Timestamp{0ms});
    const CanFrame bad{msg::engine_telemetry_id, {255, 255}, 4, Timestamp{10ms}};
    EXPECT_EQ(bus.send(bad).delivered, 1U);
    dispatch(*diag, monitor, Timestamp{10ms});
    EXPECT_EQ(bus.send(bad).delivered, 1U);
    dispatch(*diag, monitor, Timestamp{20ms});
    const auto active = store.record(FaultId::EnginePayloadInvalid);
    ASSERT_TRUE(active);
    EXPECT_EQ(active->status, FaultStatus::Active);
    EXPECT_EQ(active->occurrence_count, 1U);
    EXPECT_EQ(store.pending_events(), 1U);
    monitor.on_tick(Timestamp{500ms});
    ASSERT_TRUE(store.record(FaultId::EngineMessageMissing));
    EXPECT_EQ(store.record(FaultId::EngineMessageMissing)->status, FaultStatus::Active);
    EXPECT_EQ(bus.send(msg::encode_engine({}, Timestamp{500ms})).delivered, 1U);
    dispatch(*diag, monitor, Timestamp{500ms});
    const auto recovered = store.record(FaultId::EnginePayloadInvalid);
    ASSERT_TRUE(recovered);
    EXPECT_EQ(recovered->status, FaultStatus::Cleared);
    EXPECT_EQ(recovered->last_recovery, Timestamp{500ms});
    ASSERT_TRUE(store.record(FaultId::EngineMessageMissing));
    EXPECT_EQ(store.record(FaultId::EngineMessageMissing)->status, FaultStatus::Cleared);
    monitor.stop();
}

} // namespace
