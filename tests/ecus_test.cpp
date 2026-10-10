#include "vaecu/brake_ecu.hpp"
#include "vaecu/dashboard_ecu.hpp"
#include "vaecu/engine_ecu.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <stdexcept>

namespace {
using namespace vaecu;
using namespace std::chrono_literals;
using testing::_;
using testing::Return;
using testing::StrictMock;
namespace msg = vaecu::messages;

Timestamp at(std::chrono::milliseconds value) { return Timestamp{value}; }

class MockSender : public ICanSender {
public:
    MOCK_METHOD(SendResult, send, (const CanFrame&), (override));
};

class MockReceiver : public ICanReceiver {
public:
    MOCK_METHOD(ReceiveResult, try_receive, (), (override));
};

void verify_lifecycle(IEcu& ecu)
{
    const auto frame = msg::encode_brake({}, at(0ms));
    EXPECT_THROW(ecu.on_tick(at(0ms)), std::logic_error);
    EXPECT_THROW(ecu.on_message(frame, at(0ms)), std::logic_error);
    ecu.start(at(0ms));
    EXPECT_THROW(ecu.start(at(0ms)), std::logic_error);
    ecu.stop();
    ecu.stop();
    EXPECT_THROW(ecu.on_tick(at(1ms)), std::logic_error);
    EXPECT_THROW(ecu.on_message(frame, at(1ms)), std::logic_error);
    EXPECT_THROW(ecu.start(at(1ms)), std::logic_error);
}

TEST(EngineEcuTest, PublishesCurrentInputsOncePerTickThroughMockSender)
{
    StrictMock<MockSender> sender;
    EngineEcu ecu{sender, {3000, 88}};
    EXPECT_FALSE(ecu.last_send_result());
    ecu.start(at(0ms));
    EXPECT_CALL(sender, send(_)).WillOnce([](const CanFrame& frame) {
        EXPECT_EQ(frame.id(), CanId{0x100});
        const CanFrame::Payload expected{0xB8, 0x0B, 0x58, 0};
        EXPECT_EQ(frame.payload(), expected);
        EXPECT_EQ(frame.payload_size(), 4U);
        EXPECT_EQ(frame.timestamp(), at(20ms));
        return SendResult{SendStatus::Routed, 1, 0};
    });
    ecu.on_tick(at(20ms));
    ecu.set_inputs({1200, 45});
    EXPECT_CALL(sender, send(_)).WillOnce([](const CanFrame& frame) {
        const auto value = msg::decode_engine(frame);
        EXPECT_TRUE(value);
        if (value) {
            EXPECT_EQ(value->engine_rpm, 1200);
            EXPECT_EQ(value->vehicle_speed_kph, 45);
        }
        EXPECT_EQ(frame.timestamp(), at(40ms));
        return SendResult{SendStatus::Routed, 1, 0};
    });
    ecu.on_tick(at(40ms));
    ASSERT_TRUE(ecu.last_send_result());
    EXPECT_EQ(ecu.last_send_result()->delivered, 1U);
    ecu.stop();
}

TEST(EngineEcuTest, RejectsInvalidInputsWithoutReplacingExistingState)
{
    StrictMock<MockSender> sender;
    EXPECT_THROW((void)(EngineEcu{sender, {9001, 0}}), std::invalid_argument);
    EXPECT_THROW((void)(EngineEcu{sender, {0, 301}}), std::invalid_argument);
    EngineEcu ecu{sender};
    ecu.set_inputs({9000, 300});
    EXPECT_THROW(ecu.set_inputs({9001, 0}), std::invalid_argument);
    EXPECT_THROW(ecu.set_inputs({0, 301}), std::invalid_argument);
    EXPECT_EQ(ecu.inputs().engine_rpm, 9000);
    EXPECT_EQ(ecu.inputs().vehicle_speed_kph, 300);
    ecu.stop();
    EXPECT_THROW(ecu.set_inputs({}), std::logic_error);
}

TEST(EngineEcuTest, ExposesOverflowAndClosureWithoutRetrying)
{
    StrictMock<MockSender> sender;
    EngineEcu ecu{sender};
    ecu.start(at(0ms));
    EXPECT_CALL(sender, send(_))
        .WillOnce(Return(SendResult{SendStatus::Routed, 1, 2}))
        .WillOnce(Return(SendResult{SendStatus::Closed, 0, 0}));
    ecu.on_tick(at(0ms));
    ASSERT_TRUE(ecu.last_send_result());
    EXPECT_EQ(ecu.last_send_result()->dropped, 2U);
    ecu.on_tick(at(1ms));
    EXPECT_EQ(ecu.last_send_result()->status, SendStatus::Closed);
    ecu.stop();
}

TEST(EngineEcuTest, UnsupportedMessagesCannotModifyInputsOrTransmit)
{
    StrictMock<MockSender> sender;
    EngineEcu ecu{sender, {1500, 20}};
    ecu.start(at(0ms));
    ecu.on_message(msg::encode_engine({8000, 200}, at(0ms)), at(0ms));
    EXPECT_EQ(ecu.last_message_result(), MessageDisposition::UnsupportedId);
    EXPECT_EQ(ecu.inputs().engine_rpm, 1500);
    EXPECT_EQ(ecu.inputs().vehicle_speed_kph, 20);
    ecu.stop();
}

TEST(EngineEcuTest, EnforcesLifecycle) {
    StrictMock<MockSender> sender;
    EngineEcu ecu{sender};
    verify_lifecycle(ecu);
}

TEST(EngineEcuTest, SenderExceptionsPropagateWithoutInventingASendResult)
{
    StrictMock<MockSender> sender;
    EngineEcu ecu{sender};
    ecu.start(at(0ms));
    EXPECT_CALL(sender, send(_)).WillOnce(testing::Throw(std::runtime_error{"send failed"}));
    EXPECT_THROW(ecu.on_tick(at(0ms)), std::runtime_error);
    EXPECT_FALSE(ecu.last_send_result());
    ecu.stop();
}

TEST(EngineEcuTest, RejectsBackwardCallbacksBeforeSendingOrChangingMessageOutcome)
{
    StrictMock<MockSender> sender;
    EngineEcu ecu{sender};
    ecu.start(at(10ms));
    const auto frame = msg::encode_brake({}, at(9ms));
    EXPECT_THROW(ecu.on_tick(at(9ms)), std::invalid_argument);
    EXPECT_THROW(ecu.on_message(frame, at(9ms)), std::invalid_argument);
    EXPECT_FALSE(ecu.last_send_result());
    EXPECT_EQ(ecu.last_message_result(), MessageDisposition::None);

    EXPECT_CALL(sender, send(_)).WillOnce(Return(SendResult{SendStatus::Routed, 1, 0}));
    ecu.on_tick(at(20ms));
    EXPECT_THROW(ecu.on_tick(at(19ms)), std::invalid_argument);
    EXPECT_THROW(ecu.on_message(frame, at(19ms)), std::invalid_argument);
    ASSERT_TRUE(ecu.last_send_result());
    EXPECT_EQ(ecu.last_send_result()->delivered, 1U);
    EXPECT_EQ(ecu.last_message_result(), MessageDisposition::None);
    // Equal processing times are allowed; the timeline need not advance per call.
    EXPECT_CALL(sender, send(_)).WillOnce(Return(SendResult{SendStatus::Routed, 1, 0}));
    ecu.on_tick(at(20ms));
    ecu.stop();
}

TEST(EngineEcuTest, FailedSendPreservesCompletedResultAndDoesNotPermitTimeRewind)
{
    StrictMock<MockSender> sender;
    EngineEcu ecu{sender};
    ecu.start(at(0ms));
    EXPECT_CALL(sender, send(_))
        .WillOnce(Return(SendResult{SendStatus::Routed, 1, 2}))
        .WillOnce(testing::Throw(std::runtime_error{"send failed"}));
    ecu.on_tick(at(10ms));
    EXPECT_THROW(ecu.on_tick(at(20ms)), std::runtime_error);
    EXPECT_THROW(ecu.on_tick(at(19ms)), std::invalid_argument);
    ASSERT_TRUE(ecu.last_send_result());
    EXPECT_EQ(ecu.last_send_result()->status, SendStatus::Routed);
    EXPECT_EQ(ecu.last_send_result()->delivered, 1U);
    EXPECT_EQ(ecu.last_send_result()->dropped, 2U);
    ecu.stop();
}

TEST(BrakeEcuTest, ApplyAndReleaseEventsChangeTheNextPublishedStatus)
{
    StrictMock<MockSender> sender;
    BrakeEcu ecu{sender};
    ecu.start(at(0ms));
    EXPECT_FALSE(ecu.brake_state().applied);
    ecu.on_message(msg::encode_brake_event(msg::BrakeEvent::Apply, at(5ms)), at(5ms));
    EXPECT_EQ(ecu.last_message_result(), MessageDisposition::Accepted);
    EXPECT_TRUE(ecu.brake_state().applied);
    EXPECT_CALL(sender, send(_)).WillOnce([](const CanFrame& frame) {
        EXPECT_EQ(frame.id(), CanId{0x200});
        EXPECT_EQ(frame.payload_size(), 1U);
        EXPECT_EQ(frame.payload()[0], 1);
        EXPECT_EQ(frame.timestamp(), at(10ms));
        return SendResult{SendStatus::Routed, 1, 0};
    });
    ecu.on_tick(at(10ms));
    ecu.on_message(msg::encode_brake_event(msg::BrakeEvent::Release, at(15ms)), at(15ms));
    EXPECT_FALSE(ecu.brake_state().applied);
    EXPECT_CALL(sender, send(_)).WillOnce([](const CanFrame& frame) {
        EXPECT_EQ(frame.payload()[0], 0);
        return SendResult{SendStatus::Routed, 1, 0};
    });
    ecu.on_tick(at(20ms));
    ecu.stop();
}

TEST(BrakeEcuTest, ConstructorCanConfigureInitiallyAppliedBrake)
{
    StrictMock<MockSender> sender;
    BrakeEcu ecu{sender, {true}};
    EXPECT_TRUE(ecu.brake_state().applied);
}

TEST(BrakeEcuTest, InvalidAndUnsupportedEventsPreserveBrakeState)
{
    StrictMock<MockSender> sender;
    BrakeEcu ecu{sender, {true}};
    ecu.start(at(0ms));
    ecu.on_message(CanFrame{msg::brake_event_id, {2}, 0, at(0ms)}, at(0ms));
    EXPECT_EQ(ecu.last_message_result(), MessageDisposition::InvalidPayload);
    ecu.on_message(CanFrame{msg::brake_event_id, {255}, 1, at(0ms)}, at(0ms));
    EXPECT_EQ(ecu.last_message_result(), MessageDisposition::InvalidPayload);
    ecu.on_message(msg::encode_brake({false}, at(0ms)), at(0ms));
    EXPECT_EQ(ecu.last_message_result(), MessageDisposition::UnsupportedId);
    EXPECT_TRUE(ecu.brake_state().applied);
    ecu.stop();
}

TEST(BrakeEcuTest, FutureAndOlderEventsCannotUndoAnAcceptedEvent)
{
    StrictMock<MockSender> sender;
    BrakeEcu ecu{sender};
    ecu.start(at(0ms));
    ecu.on_message(msg::encode_brake_event(msg::BrakeEvent::Apply, at(20ms)), at(20ms));
    ecu.on_message(msg::encode_brake_event(msg::BrakeEvent::Release, at(10ms)), at(30ms));
    EXPECT_EQ(ecu.last_message_result(), MessageDisposition::OutOfOrder);
    EXPECT_TRUE(ecu.brake_state().applied);
    ecu.on_message(msg::encode_brake_event(msg::BrakeEvent::Release, at(40ms)), at(30ms));
    EXPECT_EQ(ecu.last_message_result(), MessageDisposition::InvalidTimestamp);
    EXPECT_TRUE(ecu.brake_state().applied);
    ecu.stop();
}

TEST(BrakeEcuTest, EqualTimestampEventsFollowDispatchOrder)
{
    StrictMock<MockSender> sender;
    BrakeEcu ecu{sender};
    ecu.start(at(0ms));
    ecu.on_message(msg::encode_brake_event(msg::BrakeEvent::Apply, at(0ms)), at(0ms));
    ecu.on_message(msg::encode_brake_event(msg::BrakeEvent::Release, at(0ms)), at(0ms));
    EXPECT_EQ(ecu.last_message_result(), MessageDisposition::Accepted);
    EXPECT_FALSE(ecu.brake_state().applied);
    ecu.stop();
}

TEST(BrakeEcuTest, ReportsPartialDeliveryAndClosedTransport)
{
    StrictMock<MockSender> sender;
    BrakeEcu ecu{sender};
    ecu.start(at(0ms));
    EXPECT_CALL(sender, send(_))
        .WillOnce(Return(SendResult{SendStatus::Routed, 1, 1}))
        .WillOnce(Return(SendResult{SendStatus::Closed, 0, 0}));
    ecu.on_tick(at(0ms));
    ASSERT_TRUE(ecu.last_send_result());
    EXPECT_EQ(ecu.last_send_result()->dropped, 1U);
    ecu.on_tick(at(1ms));
    EXPECT_EQ(ecu.last_send_result()->status, SendStatus::Closed);
    ecu.stop();
}

TEST(BrakeEcuTest, EnforcesLifecycle) {
    StrictMock<MockSender> sender;
    BrakeEcu ecu{sender};
    verify_lifecycle(ecu);
}

TEST(BrakeEcuTest, RejectsBackwardCallbacksBeforeSendingOrApplyingEvents)
{
    StrictMock<MockSender> sender;
    BrakeEcu ecu{sender};
    ecu.start(at(10ms));
    EXPECT_THROW(ecu.on_tick(at(9ms)), std::invalid_argument);
    EXPECT_THROW(ecu.on_message(msg::encode_brake_event(msg::BrakeEvent::Apply,
        at(9ms)), at(9ms)), std::invalid_argument);
    EXPECT_FALSE(ecu.brake_state().applied);
    EXPECT_FALSE(ecu.last_send_result());
    EXPECT_EQ(ecu.last_message_result(), MessageDisposition::None);

    ecu.on_message(msg::encode_brake_event(msg::BrakeEvent::Apply, at(20ms)), at(20ms));
    EXPECT_THROW(ecu.on_message(msg::encode_brake_event(msg::BrakeEvent::Release,
        at(19ms)), at(19ms)), std::invalid_argument);
    EXPECT_THROW(ecu.on_tick(at(19ms)), std::invalid_argument);
    EXPECT_TRUE(ecu.brake_state().applied);
    EXPECT_EQ(ecu.last_message_result(), MessageDisposition::Accepted);
    EXPECT_FALSE(ecu.last_send_result());
    EXPECT_CALL(sender, send(_)).WillOnce([](const CanFrame& frame) {
        EXPECT_EQ(frame.timestamp(), at(20ms));
        EXPECT_EQ(frame.payload()[0], 1);
        return SendResult{SendStatus::Routed, 1, 0};
    });
    ecu.on_tick(at(20ms));
    ecu.stop();
}

TEST(BrakeEcuTest, FailedSendPreservesCompletedResultAndDoesNotPermitTimeRewind)
{
    StrictMock<MockSender> sender;
    BrakeEcu ecu{sender};
    ecu.start(at(0ms));
    EXPECT_CALL(sender, send(_))
        .WillOnce(Return(SendResult{SendStatus::Routed, 1, 1}))
        .WillOnce(testing::Throw(std::runtime_error{"send failed"}));
    ecu.on_tick(at(10ms));
    EXPECT_THROW(ecu.on_tick(at(20ms)), std::runtime_error);
    EXPECT_THROW(ecu.on_tick(at(19ms)), std::invalid_argument);
    ASSERT_TRUE(ecu.last_send_result());
    EXPECT_EQ(ecu.last_send_result()->status, SendStatus::Routed);
    EXPECT_EQ(ecu.last_send_result()->delivered, 1U);
    EXPECT_EQ(ecu.last_send_result()->dropped, 1U);
    ecu.stop();
}

TEST(DashboardEcuTest, StartsWithMissingSignalsAndNoInventedValues)
{
    DashboardEcu ecu;
    ecu.start(at(0ms));
    ecu.on_tick(at(1000ms));
    const auto state = ecu.state();
    EXPECT_FALSE(state.engine);
    EXPECT_FALSE(state.brake);
    EXPECT_EQ(state.engine_health, SignalHealth::Missing);
    EXPECT_EQ(state.brake_health, SignalHealth::Missing);
    ecu.stop();
}

TEST(DashboardEcuTest, ConsumesFramesDispatchedFromAMockReceiver)
{
    StrictMock<MockReceiver> receiver;
    DashboardEcu ecu;
    ecu.start(at(0ms));
    EXPECT_CALL(receiver, try_receive())
        .WillOnce(Return(ReceiveResult{msg::encode_engine({2000, 80}, at(10ms))}))
        .WillOnce(Return(ReceiveResult{msg::encode_brake({true}, at(10ms))}));
    for (int index = 0; index < 2; ++index) {
        const auto received = receiver.try_receive();
        ASSERT_TRUE(std::holds_alternative<CanFrame>(received));
        ecu.on_message(std::get<CanFrame>(received), at(20ms));
    }
    auto snapshot = ecu.state();
    ASSERT_TRUE(snapshot.engine);
    ASSERT_TRUE(snapshot.brake);
    EXPECT_EQ(snapshot.engine->engine_rpm, 2000);
    EXPECT_EQ(snapshot.engine->vehicle_speed_kph, 80);
    EXPECT_TRUE(snapshot.brake->applied);
    EXPECT_EQ(snapshot.engine_health, SignalHealth::Fresh);
    EXPECT_EQ(snapshot.brake_health, SignalHealth::Fresh);
    snapshot.engine->engine_rpm = 999;
    EXPECT_EQ(ecu.state().engine->engine_rpm, 2000);
    ecu.stop();
}

TEST(DashboardEcuTest, UsesIndependentTimeoutsAndTreatsTheBoundaryAsStale)
{
    DashboardEcu ecu{{100ms, 200ms}};
    ecu.start(at(0ms));
    ecu.on_message(msg::encode_engine({1500, 60}, at(10ms)), at(10ms));
    ecu.on_message(msg::encode_brake({true}, at(10ms)), at(10ms));
    ecu.on_tick(at(109ms));
    EXPECT_EQ(ecu.state().engine_health, SignalHealth::Fresh);
    ecu.on_tick(at(110ms));
    EXPECT_EQ(ecu.state().engine_health, SignalHealth::Stale);
    EXPECT_EQ(ecu.state().brake_health, SignalHealth::Fresh);
    ecu.on_tick(at(210ms));
    EXPECT_EQ(ecu.state().brake_health, SignalHealth::Stale);
    ASSERT_TRUE(ecu.state().engine);
    EXPECT_EQ(ecu.state().engine->engine_rpm, 1500);
    ecu.stop();
}

TEST(DashboardEcuTest, DelayedArrivalDoesNotMakeAnOldPublicationFresh)
{
    DashboardEcu ecu{{100ms, 100ms}};
    ecu.start(at(0ms));
    ecu.on_message(msg::encode_engine({1000, 10}, at(0ms)), at(500ms));
    EXPECT_EQ(ecu.last_message_result(), MessageDisposition::Accepted);
    EXPECT_EQ(ecu.state().engine_health, SignalHealth::Stale);
    EXPECT_EQ(ecu.state().brake_health, SignalHealth::Missing);
    ecu.stop();
}

TEST(DashboardEcuTest, NewValidMessagesRecoverOnlyTheirOwnSignal)
{
    DashboardEcu ecu{{100ms, 100ms}};
    ecu.start(at(0ms));
    ecu.on_message(msg::encode_engine({1000, 10}, at(0ms)), at(0ms));
    ecu.on_message(msg::encode_brake({true}, at(0ms)), at(0ms));
    ecu.on_tick(at(100ms));
    ecu.on_message(msg::encode_engine({2000, 20}, at(110ms)), at(110ms));
    EXPECT_EQ(ecu.state().engine_health, SignalHealth::Fresh);
    EXPECT_EQ(ecu.state().brake_health, SignalHealth::Stale);
    ecu.on_message(msg::encode_brake({false}, at(120ms)), at(120ms));
    EXPECT_EQ(ecu.state().brake_health, SignalHealth::Fresh);
    EXPECT_FALSE(ecu.state().brake->applied);
    ecu.stop();
}

TEST(DashboardEcuTest, MalformedMessagesDoNotRefreshOrReplaceData)
{
    DashboardEcu ecu{{100ms, 100ms}};
    ecu.start(at(0ms));
    ecu.on_message(msg::encode_engine({1000, 10}, at(0ms)), at(0ms));
    ecu.on_message(msg::encode_brake({true}, at(0ms)), at(0ms));
    ecu.on_message(CanFrame{msg::engine_telemetry_id, {0xFF, 0xFF}, 4, at(99ms)}, at(99ms));
    EXPECT_EQ(ecu.last_message_result(), MessageDisposition::InvalidPayload);
    ecu.on_message(CanFrame{msg::engine_telemetry_id, {}, 3, at(100ms)}, at(100ms));
    EXPECT_EQ(ecu.last_message_result(), MessageDisposition::InvalidPayload);
    ecu.on_message(CanFrame{msg::brake_status_id, {2}, 1, at(100ms)}, at(100ms));
    EXPECT_EQ(ecu.last_message_result(), MessageDisposition::InvalidPayload);
    EXPECT_EQ(ecu.state().engine->engine_rpm, 1000);
    EXPECT_TRUE(ecu.state().brake->applied);
    EXPECT_EQ(ecu.state().engine_health, SignalHealth::Stale);
    EXPECT_EQ(ecu.state().brake_health, SignalHealth::Stale);
    ecu.stop();
}

TEST(DashboardEcuTest, UnsupportedMessagesStillAdvanceFreshness)
{
    DashboardEcu ecu{{100ms, 100ms}};
    ecu.start(at(0ms));
    ecu.on_message(msg::encode_engine({1000, 10}, at(0ms)), at(0ms));
    ecu.on_message(msg::encode_brake_event(msg::BrakeEvent::Apply, at(100ms)), at(100ms));
    EXPECT_EQ(ecu.last_message_result(), MessageDisposition::UnsupportedId);
    EXPECT_EQ(ecu.state().engine_health, SignalHealth::Stale);
    EXPECT_FALSE(ecu.state().brake);
    ecu.stop();
}

TEST(DashboardEcuTest, FutureAndOutOfOrderFramesCannotRefreshSignals)
{
    DashboardEcu ecu{{100ms, 100ms}};
    ecu.start(at(0ms));
    ecu.on_message(msg::encode_engine({1000, 10}, at(20ms)), at(20ms));
    ecu.on_message(msg::encode_engine({2000, 20}, at(200ms)), at(120ms));
    EXPECT_EQ(ecu.last_message_result(), MessageDisposition::InvalidTimestamp);
    ecu.on_message(msg::encode_engine({2000, 20}, at(10ms)), at(120ms));
    EXPECT_EQ(ecu.last_message_result(), MessageDisposition::OutOfOrder);
    EXPECT_EQ(ecu.state().engine->engine_rpm, 1000);
    EXPECT_EQ(ecu.state().engine_health, SignalHealth::Stale);
    ecu.stop();
}

TEST(DashboardEcuTest, DuplicateTimestampsDoNotExtendFreshness)
{
    DashboardEcu ecu{{100ms, 100ms}};
    ecu.start(at(0ms));
    ecu.on_message(msg::encode_brake({true}, at(0ms)), at(0ms));
    ecu.on_message(msg::encode_brake({false}, at(0ms)), at(100ms));
    EXPECT_EQ(ecu.last_message_result(), MessageDisposition::Accepted);
    EXPECT_EQ(ecu.state().brake_health, SignalHealth::Stale);
    EXPECT_FALSE(ecu.state().brake->applied);
    ecu.stop();
}

TEST(DashboardEcuTest, RejectsNonPositiveTimeouts)
{
    EXPECT_THROW((void)(DashboardEcu{{0ms, 100ms}}), std::invalid_argument);
    EXPECT_THROW((void)(DashboardEcu{{100ms, -1ms}}), std::invalid_argument);
}

TEST(DashboardEcuTest, RejectsProcessingTimeRegressionWithoutChangingData)
{
    DashboardEcu ecu;
    ecu.start(at(10ms));
    EXPECT_THROW(ecu.on_tick(at(9ms)), std::invalid_argument);
    ecu.on_message(msg::encode_brake({true}, at(20ms)), at(20ms));
    EXPECT_THROW(ecu.on_message(msg::encode_brake({false}, at(10ms)), at(10ms)),
        std::invalid_argument);
    EXPECT_TRUE(ecu.state().brake->applied);
    ecu.stop();
}

TEST(DashboardEcuTest, HandlesExtremeSyntheticTimesWithoutDurationOverflow)
{
    DashboardEcu ecu;
    ecu.start(Timestamp::min());
    ecu.on_message(msg::encode_brake({true}, Timestamp::min()), Timestamp::min());
    ecu.on_tick(Timestamp::max());
    EXPECT_EQ(ecu.state().brake_health, SignalHealth::Stale);
    ecu.on_message(msg::encode_brake({false}, Timestamp::max()), Timestamp::max());
    EXPECT_EQ(ecu.state().brake_health, SignalHealth::Fresh);
    ecu.stop();
}

TEST(DashboardEcuTest, StoppedStateDoesNotExposeRetainedDataAsFresh)
{
    DashboardEcu ecu;
    ecu.start(at(0ms));
    ecu.on_message(msg::encode_engine({1000, 10}, at(0ms)), at(0ms));
    ecu.stop();
    EXPECT_EQ(ecu.state().engine_health, SignalHealth::Stale);
    EXPECT_EQ(ecu.state().brake_health, SignalHealth::Missing);
}

TEST(DashboardEcuTest, EnforcesLifecycle) {
    DashboardEcu ecu;
    verify_lifecycle(ecu);
}

TEST(EcuLifecycleTest, StopBeforeStartPermanentlyPreventsStart)
{
    StrictMock<MockSender> sender;
    EngineEcu engine{sender};
    BrakeEcu brake{sender};
    DashboardEcu dashboard;
    for (IEcu* ecu : {static_cast<IEcu*>(&engine), static_cast<IEcu*>(&brake),
                     static_cast<IEcu*>(&dashboard)}) {
        ecu->stop();
        ecu->stop();
        EXPECT_THROW(ecu->start(at(0ms)), std::logic_error);
    }
}

} // namespace
