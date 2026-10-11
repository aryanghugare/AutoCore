#include "vaecu/brake_ecu.hpp"
#include "vaecu/dashboard_ecu.hpp"
#include "vaecu/diagnostics.hpp"
#include "vaecu/engine_ecu.hpp"
#include "vaecu/virtual_can_bus.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <optional>
#include <stdexcept>

namespace {
using namespace vaecu;
using namespace std::chrono_literals;
namespace msg = vaecu::messages;

Timestamp at(std::chrono::milliseconds value) { return Timestamp{value}; }

void dispatch(ICanReceiver& inbox, IEcu& ecu, Timestamp now)
{
    for (;;) {
        const auto received = inbox.try_receive();
        const auto* frame = std::get_if<CanFrame>(&received);
        if (!frame) return;
        ecu.on_message(*frame, now);
    }
}

class PlatformIntegrationTest : public testing::Test {
protected:
    void SetUp() override
    {
        commands = bus.register_receiver("brake", {msg::brake_event_id});
        display = bus.register_receiver("dashboard",
            {msg::engine_telemetry_id, msg::brake_status_id});
        diagnostic = bus.register_receiver("diagnostics",
            {msg::engine_telemetry_id, msg::brake_status_id, msg::brake_event_id});
        engine.start(at(0ms));
        brake.start(at(0ms));
        dashboard.start(at(0ms));
        monitor.start(at(0ms));
    }

    void TearDown() override
    {
        dashboard.stop();
        monitor.stop();
        brake.stop();
        engine.stop();
        (void)bus.close();
        store.close_events();
    }

    void pump(Timestamp now)
    {
        dispatch(*commands, brake, now);
        dispatch(*display, dashboard, now);
        dispatch(*diagnostic, monitor, now);
    }

    void publish(Timestamp now)
    {
        engine.on_tick(now);
        brake.on_tick(now);
        pump(now);
    }

    void tick_observers(Timestamp now)
    {
        dashboard.on_tick(now);
        monitor.on_tick(now);
    }

    void expect_fault(FaultId id, FaultStatus status, Timestamp occurrence,
                      std::optional<Timestamp> recovery = std::nullopt)
    {
        const auto record = store.record(id);
        ASSERT_TRUE(record);
        EXPECT_EQ(record->status, status);
        EXPECT_EQ(record->first_occurrence, occurrence);
        EXPECT_EQ(record->last_recovery, recovery);
        EXPECT_EQ(record->occurrence_count, 1U);
    }

    VirtualCanBus bus;
    DiagnosticStore store;
    EngineEcu engine{bus, {1500, 60}};
    BrakeEcu brake{bus};
    DashboardEcu dashboard{{50ms, 50ms}};
    DiagnosticMonitor monitor{store, {100ms, 50ms, 200ms}};
    std::shared_ptr<CanReceiver> commands, display, diagnostic;
};

TEST_F(PlatformIntegrationTest, BoundaryTelemetryAndApplyReleaseFlowThroughAllThreeEcus)
{
    engine.set_inputs({0, 0});
    publish(at(0ms));
    ASSERT_TRUE(dashboard.state().engine);
    ASSERT_TRUE(dashboard.state().brake);
    EXPECT_EQ(dashboard.state().engine->engine_rpm, 0);
    EXPECT_EQ(dashboard.state().engine->vehicle_speed_kph, 0);
    EXPECT_FALSE(dashboard.state().brake->applied);

    const auto apply = bus.send(msg::encode_brake_event(msg::BrakeEvent::Apply, at(10ms)));
    EXPECT_EQ(apply.status, SendStatus::Routed);
    EXPECT_EQ(apply.delivered, 2U); // Brake + diagnostics, never Dashboard.
    EXPECT_EQ(apply.dropped, 0U);
    EXPECT_EQ(display->pending(), 0U);
    pump(at(10ms));
    EXPECT_TRUE(brake.brake_state().applied);
    EXPECT_FALSE(dashboard.state().brake->applied); // Waits for Brake telemetry.
    engine.set_inputs({9000, 300});
    publish(at(10ms));
    ASSERT_TRUE(dashboard.state().engine);
    ASSERT_TRUE(dashboard.state().brake);
    EXPECT_EQ(dashboard.state().engine->engine_rpm, 9000);
    EXPECT_EQ(dashboard.state().engine->vehicle_speed_kph, 300);
    EXPECT_TRUE(dashboard.state().brake->applied);

    EXPECT_EQ(bus.send(msg::encode_brake_event(msg::BrakeEvent::Release, at(20ms))).delivered, 2U);
    pump(at(20ms));
    publish(at(20ms));
    EXPECT_FALSE(dashboard.state().brake->applied);
    EXPECT_EQ(dashboard.state().engine_health, SignalHealth::Fresh);
    EXPECT_EQ(dashboard.state().brake_health, SignalHealth::Fresh);
    EXPECT_EQ(store.pending_events(), 0U);
    EXPECT_EQ(commands->pending() + display->pending() + diagnostic->pending(), 0U);
}

TEST_F(PlatformIntegrationTest, MalformedSignalsAndCommandsPreserveDataSuppressDuplicatesAndRecover)
{
    publish(at(0ms));
    const CanFrame bad_engine{msg::engine_telemetry_id, {255, 255}, 4, at(10ms)};
    const CanFrame bad_brake{msg::brake_status_id, {2}, 1, at(10ms)};
    const CanFrame bad_command{msg::brake_event_id, {255}, 1, at(10ms)};
    for (const auto now : {at(10ms), at(20ms)}) {
        for (const auto& frame : {bad_engine, bad_brake, bad_command}) {
            const auto sent = bus.send(frame);
            EXPECT_EQ(sent.status, SendStatus::Routed); // Valid envelope, bad schema.
            EXPECT_EQ(sent.delivered, 2U);
            EXPECT_EQ(sent.dropped, 0U);
        }
        pump(now);
        EXPECT_EQ(brake.last_message_result(), MessageDisposition::InvalidPayload);
        EXPECT_EQ(dashboard.last_message_result(), MessageDisposition::InvalidPayload);
    }
    ASSERT_TRUE(dashboard.state().engine);
    ASSERT_TRUE(dashboard.state().brake);
    EXPECT_EQ(dashboard.state().engine->engine_rpm, 1500);
    EXPECT_FALSE(dashboard.state().brake->applied);
    EXPECT_FALSE(brake.brake_state().applied);
    EXPECT_EQ(store.pending_events(), 3U);
    for (const auto id : {FaultId::EnginePayloadInvalid, FaultId::BrakePayloadInvalid,
                          FaultId::BrakeEventPayloadInvalid}) {
        expect_fault(id, FaultStatus::Active, at(10ms));
    }
    tick_observers(at(50ms));
    EXPECT_EQ(dashboard.state().engine_health, SignalHealth::Stale);
    EXPECT_EQ(dashboard.state().brake_health, SignalHealth::Stale);
    EXPECT_EQ(bus.send(msg::encode_brake_event(msg::BrakeEvent::Apply, at(60ms))).delivered, 2U);
    pump(at(60ms));
    engine.set_inputs({2200, 80});
    publish(at(60ms));
    ASSERT_TRUE(dashboard.state().engine);
    ASSERT_TRUE(dashboard.state().brake);
    EXPECT_EQ(dashboard.state().engine->engine_rpm, 2200);
    EXPECT_TRUE(dashboard.state().brake->applied);
    EXPECT_EQ(dashboard.state().engine_health, SignalHealth::Fresh);
    EXPECT_EQ(dashboard.state().brake_health, SignalHealth::Fresh);
    for (const auto id : {FaultId::EnginePayloadInvalid, FaultId::BrakePayloadInvalid,
                          FaultId::BrakeEventPayloadInvalid}) {
        expect_fault(id, FaultStatus::Cleared, at(10ms), at(60ms));
    }
    EXPECT_FALSE(store.record(FaultId::EngineCommunicationTimeout));
}

TEST_F(PlatformIntegrationTest, MissingEngineHasInclusiveTimeoutsAndRecoversWhileBrakeStaysHealthy)
{
    brake.on_tick(at(0ms));
    pump(at(0ms));
    EXPECT_EQ(bus.send(CanFrame{CanId{0x7FF}, {}, 0, at(99ms)}).delivered, 0U);
    brake.on_tick(at(99ms));
    pump(at(99ms));
    EXPECT_FALSE(store.record(FaultId::EngineMessageMissing));
    tick_observers(at(100ms));
    expect_fault(FaultId::EngineMessageMissing, FaultStatus::Active, at(100ms));
    EXPECT_EQ(dashboard.state().engine_health, SignalHealth::Missing);
    EXPECT_FALSE(dashboard.state().engine);
    brake.on_tick(at(199ms));
    pump(at(199ms));
    EXPECT_FALSE(store.record(FaultId::EngineCommunicationTimeout));
    tick_observers(at(200ms));
    tick_observers(at(200ms));
    expect_fault(FaultId::EngineCommunicationTimeout, FaultStatus::Active, at(200ms));
    EXPECT_FALSE(store.record(FaultId::EngineMessageStale));
    EXPECT_FALSE(store.record(FaultId::BrakeCommunicationTimeout));
    EXPECT_EQ(store.pending_events(), 2U);
    engine.on_tick(at(201ms));
    pump(at(201ms));
    expect_fault(FaultId::EngineMessageMissing, FaultStatus::Cleared, at(100ms), at(201ms));
    expect_fault(FaultId::EngineCommunicationTimeout, FaultStatus::Cleared, at(200ms), at(201ms));
    EXPECT_EQ(dashboard.state().engine_health, SignalHealth::Fresh);
    EXPECT_EQ(dashboard.state().brake_health, SignalHealth::Fresh);
    EXPECT_EQ(store.pending_events(), 4U);
}

TEST_F(PlatformIntegrationTest, ReplayedAndFutureFramesCannotReplaceStateOrExtendFreshness)
{
    publish(at(0ms));
    EXPECT_EQ(bus.send(msg::encode_brake_event(msg::BrakeEvent::Apply, at(20ms))).delivered, 2U);
    pump(at(20ms));
    engine.set_inputs({2500, 80});
    publish(at(20ms));
    for (const auto timestamp : {at(10ms), at(200ms)}) {
        EXPECT_EQ(bus.send(msg::encode_engine({8000, 200}, timestamp)).delivered, 2U);
        EXPECT_EQ(bus.send(msg::encode_brake_event(msg::BrakeEvent::Release, timestamp)).delivered, 2U);
        pump(at(30ms));
        const auto expected = timestamp == at(10ms) ? MessageDisposition::OutOfOrder :
            MessageDisposition::InvalidTimestamp;
        EXPECT_EQ(dashboard.last_message_result(), expected);
        EXPECT_EQ(brake.last_message_result(), expected);
    }
    ASSERT_TRUE(dashboard.state().engine);
    ASSERT_TRUE(dashboard.state().brake);
    EXPECT_EQ(dashboard.state().engine->engine_rpm, 2500);
    EXPECT_TRUE(brake.brake_state().applied);
    EXPECT_TRUE(dashboard.state().brake->applied);
    EXPECT_FALSE(store.record(FaultId::EnginePayloadInvalid));
    EXPECT_FALSE(store.record(FaultId::BrakeEventPayloadInvalid));
    tick_observers(at(69ms));
    EXPECT_EQ(dashboard.state().engine_health, SignalHealth::Fresh);
    tick_observers(at(70ms));
    EXPECT_EQ(dashboard.state().engine_health, SignalHealth::Stale);
    expect_fault(FaultId::EngineMessageStale, FaultStatus::Active, at(70ms));
    brake.on_tick(at(219ms));
    pump(at(219ms));
    tick_observers(at(229ms));
    EXPECT_FALSE(store.record(FaultId::EngineCommunicationTimeout));
    tick_observers(at(230ms)); // Rejected traffic was activity at 30 ms.
    expect_fault(FaultId::EngineCommunicationTimeout, FaultStatus::Active, at(230ms));
    EXPECT_EQ(bus.send(msg::encode_brake_event(msg::BrakeEvent::Release, at(231ms))).delivered, 2U);
    pump(at(231ms));
    publish(at(231ms));
    EXPECT_FALSE(dashboard.state().brake->applied);
    EXPECT_EQ(dashboard.state().engine_health, SignalHealth::Fresh);
    expect_fault(FaultId::EngineMessageStale, FaultStatus::Cleared, at(70ms), at(231ms));
    expect_fault(FaultId::EngineCommunicationTimeout, FaultStatus::Cleared, at(230ms), at(231ms));
}

TEST_F(PlatformIntegrationTest, ShutdownDiscardsPendingCommandsAndTelemetryWithoutApplyingThem)
{
    publish(at(0ms));
    engine.set_inputs({8000, 200});
    engine.on_tick(at(10ms));
    EXPECT_EQ(bus.send(msg::encode_brake_event(msg::BrakeEvent::Apply, at(10ms))).delivered, 2U);
    brake.on_tick(at(10ms));
    EXPECT_EQ(commands->pending(), 1U);
    EXPECT_EQ(display->pending(), 2U);
    EXPECT_EQ(diagnostic->pending(), 3U);
    EXPECT_EQ(bus.close(), 6U);
    EXPECT_EQ(bus.close(), 0U);
    pump(at(20ms));
    engine.on_tick(at(20ms));
    brake.on_tick(at(20ms));
    ASSERT_TRUE(engine.last_send_result());
    ASSERT_TRUE(brake.last_send_result());
    EXPECT_EQ(engine.last_send_result()->status, SendStatus::Closed);
    EXPECT_EQ(brake.last_send_result()->status, SendStatus::Closed);
    ASSERT_TRUE(dashboard.state().engine);
    ASSERT_TRUE(dashboard.state().brake);
    EXPECT_EQ(dashboard.state().engine->engine_rpm, 1500);
    EXPECT_FALSE(brake.brake_state().applied);
    EXPECT_FALSE(dashboard.state().brake->applied);
    dashboard.stop();
    EXPECT_EQ(dashboard.state().engine_health, SignalHealth::Stale);
    EXPECT_EQ(dashboard.state().brake_health, SignalHealth::Stale);
    EXPECT_EQ(store.pending_events(), 0U); // Shutdown is not fault recovery.
}

class MockSender : public ICanSender {
public:
    MOCK_METHOD(SendResult, send, (const CanFrame& frame), (override));
};

TEST(PlatformMockIntegrationTest, DroppedAndThrowingEngineTransportTimesOutThenRecovers)
{
    VirtualCanBus bus;
    const auto display = bus.register_receiver("dashboard",
        {msg::engine_telemetry_id, msg::brake_status_id});
    const auto diagnostic = bus.register_receiver("diagnostics",
        {msg::engine_telemetry_id, msg::brake_status_id});
    testing::StrictMock<MockSender> sender;
    DiagnosticStore store;
    DiagnosticMonitor monitor{store, {100ms, 50ms, 200ms}};
    EngineEcu engine{sender, {3000, 88}};
    BrakeEcu brake{bus};
    DashboardEcu dashboard{{50ms, 50ms}};
    engine.start(at(0ms));
    brake.start(at(0ms));
    dashboard.start(at(0ms));
    monitor.start(at(0ms));
    const auto pump = [&](Timestamp now) {
        dispatch(*display, dashboard, now);
        dispatch(*diagnostic, monitor, now);
    };
    const auto publication_at = [](Timestamp now) {
        return testing::Truly([now](const CanFrame& frame) {
            return frame.id() == msg::engine_telemetry_id && frame.timestamp() == now &&
                frame.payload_size() == 4 && frame.payload()[0] == 0xB8 &&
                frame.payload()[1] == 0x0B && frame.payload()[2] == 88 && frame.payload()[3] == 0;
        });
    };
    {
        testing::InSequence ordered;
        EXPECT_CALL(sender, send(publication_at(at(0ms))))
            .WillOnce([&bus](const CanFrame& frame) { return bus.send(frame); });
        EXPECT_CALL(sender, send(publication_at(at(199ms))))
            .WillOnce(testing::Return(SendResult{SendStatus::Routed, 0, 2}));
        EXPECT_CALL(sender, send(publication_at(at(200ms))))
            .WillOnce(testing::Throw(std::runtime_error{"simulated transport failure"}));
        EXPECT_CALL(sender, send(publication_at(at(201ms))))
            .WillOnce([&bus](const CanFrame& frame) { return bus.send(frame); });
    }
    engine.on_tick(at(0ms));
    brake.on_tick(at(0ms));
    pump(at(0ms));
    engine.on_tick(at(199ms)); // No frame reaches either observer.
    brake.on_tick(at(199ms));
    pump(at(199ms));
    EXPECT_THROW(engine.on_tick(at(200ms)), std::runtime_error);
    ASSERT_TRUE(engine.last_send_result());
    EXPECT_EQ(engine.last_send_result()->dropped, 2U); // Last completed result.
    monitor.on_tick(at(200ms));
    const auto timeout = store.record(FaultId::EngineCommunicationTimeout);
    ASSERT_TRUE(timeout);
    EXPECT_EQ(timeout->status, FaultStatus::Active);
    EXPECT_EQ(dashboard.state().engine_health, SignalHealth::Stale);
    EXPECT_EQ(dashboard.state().brake_health, SignalHealth::Fresh);
    EXPECT_FALSE(store.record(FaultId::BrakeCommunicationTimeout));
    engine.on_tick(at(201ms));
    brake.on_tick(at(201ms));
    pump(at(201ms));
    const auto recovered = store.record(FaultId::EngineCommunicationTimeout);
    ASSERT_TRUE(recovered);
    EXPECT_EQ(recovered->status, FaultStatus::Cleared);
    EXPECT_EQ(recovered->occurrence_count, 1U);
    EXPECT_EQ(recovered->last_recovery, at(201ms));
    EXPECT_EQ(dashboard.state().engine_health, SignalHealth::Fresh);
    ASSERT_TRUE(dashboard.state().engine);
    ASSERT_TRUE(engine.last_send_result());
    EXPECT_EQ(dashboard.state().engine->engine_rpm, 3000);
    EXPECT_EQ(engine.last_send_result()->delivered, 2U);
    monitor.stop();
    dashboard.stop();
    brake.stop();
    engine.stop();
    EXPECT_EQ(bus.close(), 0U);
}

} // namespace
