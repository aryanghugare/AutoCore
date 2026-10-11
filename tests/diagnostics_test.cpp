#include "vaecu/diagnostics.hpp"
#include "vaecu/messages.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <limits>
#include <stdexcept>

namespace {
using namespace vaecu;
using namespace std::chrono_literals;
using testing::StrictMock;
namespace msg = vaecu::messages;

Timestamp at(std::chrono::milliseconds duration) { return Timestamp{duration}; }
const DiagnosticConfig quick{100ms, 50ms, 200ms};

class MockSink : public IDiagnosticSink {
public:
    MOCK_METHOD(void, set_fault, (FaultId, bool, Timestamp), (override));
};

void publish_initial(DiagnosticMonitor& monitor)
{
    monitor.on_message(msg::encode_engine({1000, 10}, at(0ms)), at(0ms));
    monitor.on_message(msg::encode_brake({}, at(0ms)), at(0ms));
}

void expect_status(const DiagnosticStore& store, FaultId id, FaultStatus status)
{
    const auto record = store.record(id);
    ASSERT_TRUE(record);
    EXPECT_EQ(record->status, status);
}

TEST(DiagnosticStoreTest, InactiveUnseenFaultsDoNotCreateFaultRecords)
{
    DiagnosticStore store;
    for (const auto definition : fault_definitions) {
        EXPECT_FALSE(store.record(definition.id));
        store.set_fault(definition.id, false, at(0ms));
        EXPECT_FALSE(store.record(definition.id));
    }
    EXPECT_EQ(store.pending_events(), 0U);
    EXPECT_FALSE(store.take_event());
}

TEST(DiagnosticStoreTest, RepeatedActiveCyclesDoNotDuplicateRecordsOrEvents)
{
    DiagnosticStore store;
    const auto id = FaultId::EngineSpeedSensorInvalid;
    store.set_fault(id, true, at(10ms));
    for (int cycle = 11; cycle <= 100; ++cycle) {
        store.set_fault(id, true, at(std::chrono::milliseconds{cycle}));
    }
    auto record = store.record(id);
    ASSERT_TRUE(record);
    EXPECT_EQ(record->first_occurrence, at(10ms));
    EXPECT_EQ(record->last_occurrence, at(10ms));
    EXPECT_EQ(record->occurrence_count, 1U);
    EXPECT_FALSE(record->last_recovery);
    EXPECT_EQ(store.pending_events(), 1U);
    record->status = FaultStatus::Cleared;
    expect_status(store, id, FaultStatus::Active);
}

TEST(DiagnosticStoreTest, TracksRecoveryAndReactivationAsDistinctEpisodes)
{
    DiagnosticStore store;
    const auto id = FaultId::EnginePayloadInvalid;
    store.set_fault(id, true, at(10ms));
    store.set_fault(id, false, at(20ms));
    store.set_fault(id, false, at(25ms));
    expect_status(store, id, FaultStatus::Cleared);
    store.set_fault(id, true, at(30ms));
    const auto record = store.record(id);
    ASSERT_TRUE(record);
    EXPECT_EQ(record->status, FaultStatus::Active);
    EXPECT_EQ(record->first_occurrence, at(10ms));
    EXPECT_EQ(record->last_occurrence, at(30ms));
    EXPECT_EQ(record->last_recovery, at(20ms));
    EXPECT_EQ(record->occurrence_count, 2U);
    for (const auto expected : {
            DiagnosticEvent{id, DiagnosticEventKind::Raised, at(10ms)},
            DiagnosticEvent{id, DiagnosticEventKind::Recovered, at(20ms)},
            DiagnosticEvent{id, DiagnosticEventKind::Raised, at(30ms)}}) {
        const auto event = store.take_event();
        ASSERT_TRUE(event);
        EXPECT_EQ(event->id, expected.id);
        EXPECT_EQ(event->kind, expected.kind);
        EXPECT_EQ(event->timestamp, expected.timestamp);
    }
    EXPECT_FALSE(store.take_event());
}

TEST(DiagnosticStoreTest, FaultStatesRemainIndependent)
{
    DiagnosticStore store;
    store.set_fault(FaultId::EnginePayloadInvalid, true, at(0ms));
    store.set_fault(FaultId::BrakePayloadInvalid, true, at(0ms));
    store.set_fault(FaultId::EnginePayloadInvalid, false, at(1ms));
    expect_status(store, FaultId::EnginePayloadInvalid, FaultStatus::Cleared);
    expect_status(store, FaultId::BrakePayloadInvalid, FaultStatus::Active);
}

TEST(DiagnosticStoreTest, OverflowRetainsNewestEventsWithoutLosingFaultState)
{
    DiagnosticStore store{2};
    const auto id = FaultId::EnginePayloadInvalid;
    store.set_fault(id, true, at(0ms));
    store.set_fault(id, false, at(1ms));
    store.set_fault(id, true, at(2ms));
    store.set_fault(id, false, at(3ms));
    EXPECT_EQ(store.pending_events(), 2U);
    EXPECT_EQ(store.discarded_events(), 2U);
    const auto first = store.take_event();
    const auto second = store.take_event();
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_EQ(first->timestamp, at(2ms));
    EXPECT_EQ(first->kind, DiagnosticEventKind::Raised);
    EXPECT_EQ(second->timestamp, at(3ms));
    EXPECT_EQ(second->kind, DiagnosticEventKind::Recovered);
    EXPECT_FALSE(store.take_event());
    const auto record = store.record(id);
    ASSERT_TRUE(record);
    EXPECT_EQ(record->status, FaultStatus::Cleared);
    EXPECT_EQ(record->occurrence_count, 2U);
    EXPECT_EQ(record->last_recovery, at(3ms));
}

TEST(DiagnosticStoreTest, RingReusesConsumedSlotsInFifoOrder)
{
    DiagnosticStore store{2};
    const auto id = FaultId::BrakePayloadInvalid;
    store.set_fault(id, true, at(0ms));
    store.set_fault(id, false, at(1ms));
    ASSERT_TRUE(store.take_event());
    store.set_fault(id, true, at(2ms));
    const auto first = store.take_event();
    const auto second = store.take_event();
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_EQ(first->timestamp, at(1ms));
    EXPECT_EQ(second->timestamp, at(2ms));
    EXPECT_EQ(store.discarded_events(), 0U);
}

TEST(DiagnosticStoreTest, CapacityOneCanRetainTheLatestRecovery)
{
    DiagnosticStore store{1};
    store.set_fault(FaultId::BrakePayloadInvalid, true, at(0ms));
    store.set_fault(FaultId::BrakePayloadInvalid, false, at(1ms));
    const auto event = store.take_event();
    ASSERT_TRUE(event);
    EXPECT_EQ(event->kind, DiagnosticEventKind::Recovered);
    EXPECT_EQ(store.discarded_events(), 1U);
}

TEST(DiagnosticStoreTest, RejectsUnknownIdsAndBackwardTimeBeforeMutation)
{
    DiagnosticStore store;
    const auto unknown = static_cast<FaultId>(0xFFFF);
    EXPECT_THROW(store.set_fault(unknown, true, at(100ms)), std::invalid_argument);
    EXPECT_THROW((void)store.record(unknown), std::invalid_argument);
    store.set_fault(FaultId::BrakePayloadInvalid, true, at(10ms));
    EXPECT_THROW(store.set_fault(FaultId::BrakePayloadInvalid, false, at(9ms)),
        std::invalid_argument);
    expect_status(store, FaultId::BrakePayloadInvalid, FaultStatus::Active);
    EXPECT_EQ(store.pending_events(), 1U);
}

TEST(DiagnosticStoreTest, RejectsZeroEventCapacity)
{
    EXPECT_THROW((void)DiagnosticStore{0}, std::invalid_argument);
}

TEST(DiagnosticMonitorTest, HealthyTrafficAndSensorSamplesDoNotEmitFaults)
{
    StrictMock<MockSink> sink;
    DiagnosticMonitor monitor{sink, quick};
    monitor.start(at(0ms));
    publish_initial(monitor);
    monitor.observe_engine_inputs(0, 0, at(0ms));
    monitor.observe_engine_inputs(9000, 300, at(49ms));
    monitor.on_tick(at(49ms));
    monitor.stop();
}

TEST(DiagnosticMonitorTest, MissingStartsAtGraceBoundaryAndIsEmittedOnlyOnce)
{
    StrictMock<MockSink> sink;
    DiagnosticMonitor monitor{sink, quick};
    monitor.start(at(10ms));
    monitor.on_tick(at(109ms));
    EXPECT_CALL(sink, set_fault(FaultId::EngineMessageMissing, true, at(110ms)));
    EXPECT_CALL(sink, set_fault(FaultId::BrakeMessageMissing, true, at(110ms)));
    monitor.on_tick(at(110ms));
    monitor.on_tick(at(110ms));
    monitor.on_tick(at(111ms));
    monitor.on_tick(at(209ms));
    monitor.stop();
}

TEST(DiagnosticMonitorTest, FirstAcceptedMessagesRecoverOnlyTheirOwnMissingFaults)
{
    StrictMock<MockSink> sink;
    DiagnosticMonitor monitor{sink, quick};
    monitor.start(at(0ms));
    EXPECT_CALL(sink, set_fault(FaultId::EngineMessageMissing, true, at(100ms)));
    EXPECT_CALL(sink, set_fault(FaultId::BrakeMessageMissing, true, at(100ms)));
    monitor.on_tick(at(100ms));
    EXPECT_CALL(sink, set_fault(FaultId::EngineMessageMissing, false, at(110ms)));
    monitor.on_message(msg::encode_engine({}, at(110ms)), at(110ms));
    EXPECT_CALL(sink, set_fault(FaultId::BrakeMessageMissing, false, at(120ms)));
    monitor.on_message(msg::encode_brake({}, at(120ms)), at(120ms));
    monitor.stop();
}

TEST(DiagnosticMonitorTest, StaleAndCommunicationTimeoutHaveIndependentInclusiveBoundaries)
{
    StrictMock<MockSink> sink;
    DiagnosticMonitor monitor{sink, quick};
    monitor.start(at(0ms));
    publish_initial(monitor);
    monitor.on_tick(at(49ms));
    EXPECT_CALL(sink, set_fault(FaultId::EngineMessageStale, true, at(50ms)));
    EXPECT_CALL(sink, set_fault(FaultId::BrakeMessageStale, true, at(50ms)));
    monitor.on_tick(at(50ms));
    monitor.on_tick(at(199ms));
    EXPECT_CALL(sink, set_fault(FaultId::EngineCommunicationTimeout, true, at(200ms)));
    EXPECT_CALL(sink, set_fault(FaultId::BrakeCommunicationTimeout, true, at(200ms)));
    monitor.on_tick(at(200ms));
    monitor.on_tick(at(1000ms));
    monitor.stop();
}

TEST(DiagnosticMonitorTest, NeverReceivedStreamsBecomeMissingAndTimedOutButNotStale)
{
    DiagnosticStore store;
    DiagnosticMonitor monitor{store, quick};
    monitor.start(at(0ms));
    monitor.on_tick(at(200ms));
    expect_status(store, FaultId::EngineMessageMissing, FaultStatus::Active);
    expect_status(store, FaultId::EngineCommunicationTimeout, FaultStatus::Active);
    EXPECT_FALSE(store.record(FaultId::EngineMessageStale));
    expect_status(store, FaultId::BrakeMessageMissing, FaultStatus::Active);
    expect_status(store, FaultId::BrakeCommunicationTimeout, FaultStatus::Active);
    EXPECT_FALSE(store.record(FaultId::BrakeMessageStale));
    monitor.stop();
}

TEST(DiagnosticMonitorTest, BoundaryDeliveryEvaluatesOtherStreamsBeforeTheirNextCallback)
{
    StrictMock<MockSink> sink;
    DiagnosticMonitor monitor{sink};
    monitor.start(at(0ms));
    publish_initial(monitor);

    // A received engine publication restores that stream before evaluation.
    // Brake's queued publication has not yet been observed by this monitor.
    testing::InSequence ordered;
    EXPECT_CALL(sink, set_fault(FaultId::BrakeMessageStale, true, at(1000ms)));
    EXPECT_CALL(sink, set_fault(FaultId::BrakeCommunicationTimeout, true, at(1000ms)));
    EXPECT_CALL(sink, set_fault(FaultId::BrakeMessageStale, false, at(1000ms)));
    EXPECT_CALL(sink, set_fault(FaultId::BrakeCommunicationTimeout, false, at(1000ms)));
    monitor.on_message(msg::encode_engine({}, at(1000ms)), at(1000ms));
    monitor.on_message(msg::encode_brake({}, at(1000ms)), at(1000ms));
    monitor.on_tick(at(1000ms));
    monitor.on_tick(at(1001ms));
    monitor.stop();
}

TEST(DiagnosticMonitorTest, InvalidTrafficIsActivityButCannotRefreshUsableData)
{
    StrictMock<MockSink> sink;
    DiagnosticMonitor monitor{sink, quick};
    monitor.start(at(0ms));
    publish_initial(monitor);
    EXPECT_CALL(sink, set_fault(FaultId::EnginePayloadInvalid, true, at(100ms)));
    EXPECT_CALL(sink, set_fault(FaultId::EngineMessageStale, true, at(100ms)));
    EXPECT_CALL(sink, set_fault(FaultId::BrakeMessageStale, true, at(100ms)));
    monitor.on_message(CanFrame{msg::engine_telemetry_id, {}, 3, at(100ms)}, at(100ms));
    EXPECT_CALL(sink, set_fault(FaultId::BrakeCommunicationTimeout, true, at(200ms)));
    monitor.on_tick(at(200ms));
    monitor.on_tick(at(299ms));
    EXPECT_CALL(sink, set_fault(FaultId::EngineCommunicationTimeout, true, at(300ms)));
    monitor.on_tick(at(300ms));
    monitor.stop();
}

TEST(DiagnosticMonitorTest, InvalidFrameRecoversTimeoutButLeavesStaleAndPayloadFaultActive)
{
    DiagnosticStore store;
    DiagnosticMonitor monitor{store, quick};
    monitor.start(at(0ms));
    publish_initial(monitor);
    monitor.on_tick(at(200ms));
    monitor.on_message(CanFrame{msg::engine_telemetry_id, {255, 255}, 4, at(210ms)}, at(210ms));
    expect_status(store, FaultId::EngineCommunicationTimeout, FaultStatus::Cleared);
    expect_status(store, FaultId::EngineMessageStale, FaultStatus::Active);
    expect_status(store, FaultId::EnginePayloadInvalid, FaultStatus::Active);
    monitor.on_message(msg::encode_engine({}, at(220ms)), at(220ms));
    expect_status(store, FaultId::EngineMessageStale, FaultStatus::Cleared);
    expect_status(store, FaultId::EnginePayloadInvalid, FaultStatus::Cleared);
    expect_status(store, FaultId::BrakeCommunicationTimeout, FaultStatus::Active);
    monitor.stop();
}

TEST(DiagnosticMonitorTest, DelayedAcceptedPublicationCanRemainStale)
{
    DiagnosticStore store;
    DiagnosticMonitor monitor{store, quick};
    monitor.start(at(0ms));
    monitor.on_message(msg::encode_engine({}, at(0ms)), at(100ms));
    expect_status(store, FaultId::EngineMessageStale, FaultStatus::Active);
    EXPECT_FALSE(store.record(FaultId::EngineMessageMissing));
    EXPECT_FALSE(store.record(FaultId::EngineCommunicationTimeout));
    monitor.stop();
}

TEST(DiagnosticMonitorTest, FutureAndOutOfOrderFramesCannotClearPayloadFault)
{
    DiagnosticStore store;
    DiagnosticMonitor monitor{store, quick};
    monitor.start(at(0ms));
    publish_initial(monitor);
    monitor.on_message(CanFrame{msg::engine_telemetry_id, {}, 3, at(10ms)}, at(10ms));
    monitor.on_message(msg::encode_engine({}, at(100ms)), at(20ms));
    expect_status(store, FaultId::EnginePayloadInvalid, FaultStatus::Active);
    monitor.on_message(msg::encode_engine({}, at(-1ms)), at(30ms));
    expect_status(store, FaultId::EnginePayloadInvalid, FaultStatus::Active);
    monitor.on_message(msg::encode_engine({}, at(40ms)), at(40ms));
    expect_status(store, FaultId::EnginePayloadInvalid, FaultStatus::Cleared);
    monitor.stop();
}

TEST(DiagnosticMonitorTest, FutureAndDuplicateFramesCannotExtendPublicationFreshness)
{
    DiagnosticStore store;
    DiagnosticMonitor monitor{store, quick};
    monitor.start(at(0ms));
    publish_initial(monitor);
    monitor.on_message(msg::encode_engine({}, at(100ms)), at(49ms));
    monitor.on_message(msg::encode_engine({}, at(0ms)), at(50ms));
    expect_status(store, FaultId::EngineMessageStale, FaultStatus::Active);
    EXPECT_FALSE(store.record(FaultId::EngineCommunicationTimeout));
    monitor.stop();
}

TEST(DiagnosticMonitorTest, UnknownIdsCannotPreventCommunicationTimeout)
{
    DiagnosticStore store;
    DiagnosticMonitor monitor{store, quick};
    monitor.start(at(0ms));
    publish_initial(monitor);
    monitor.on_message(CanFrame{CanId{0x7FF}, {}, 0, at(199ms)}, at(199ms));
    monitor.on_tick(at(200ms));
    expect_status(store, FaultId::EngineCommunicationTimeout, FaultStatus::Active);
    expect_status(store, FaultId::BrakeCommunicationTimeout, FaultStatus::Active);
    monitor.stop();
}

TEST(DiagnosticMonitorTest, BrakeStatusAndEventPayloadFaultsRecoverIndependently)
{
    StrictMock<MockSink> sink;
    DiagnosticMonitor monitor{sink, quick};
    monitor.start(at(0ms));
    EXPECT_CALL(sink, set_fault(FaultId::BrakePayloadInvalid, true, at(10ms)));
    monitor.on_message(CanFrame{msg::brake_status_id, {2}, 1, at(10ms)}, at(10ms));
    EXPECT_CALL(sink, set_fault(FaultId::BrakeEventPayloadInvalid, true, at(20ms)));
    monitor.on_message(CanFrame{msg::brake_event_id, {0}, 1, at(20ms)}, at(20ms));
    EXPECT_CALL(sink, set_fault(FaultId::BrakePayloadInvalid, false, at(30ms)));
    monitor.on_message(msg::encode_brake({true}, at(30ms)), at(30ms));
    EXPECT_CALL(sink, set_fault(FaultId::BrakeEventPayloadInvalid, false, at(40ms)));
    monitor.on_message(msg::encode_brake_event(msg::BrakeEvent::Release, at(40ms)), at(40ms));
    monitor.stop();
}

TEST(DiagnosticMonitorTest, AperiodicBrakeEventsDoNotCountAsBrakeStatusActivity)
{
    DiagnosticStore store;
    DiagnosticMonitor monitor{store, quick};
    monitor.start(at(0ms));
    monitor.on_message(msg::encode_brake_event(msg::BrakeEvent::Apply, at(199ms)), at(199ms));
    monitor.on_tick(at(200ms));
    expect_status(store, FaultId::BrakeMessageMissing, FaultStatus::Active);
    expect_status(store, FaultId::BrakeCommunicationTimeout, FaultStatus::Active);
    EXPECT_FALSE(store.record(FaultId::BrakeEventPayloadInvalid));
    monitor.stop();
}

TEST(DiagnosticMonitorTest, SensorFaultsAreIndependentAndRepeatedBadSamplesAreSuppressed)
{
    StrictMock<MockSink> sink;
    DiagnosticMonitor monitor{sink};
    monitor.start(at(0ms));
    EXPECT_CALL(sink, set_fault(FaultId::EngineSpeedSensorInvalid, true, at(1ms)));
    EXPECT_CALL(sink, set_fault(FaultId::VehicleSpeedSensorInvalid, true, at(1ms)));
    monitor.observe_engine_inputs(-1, 301, at(1ms));
    monitor.observe_engine_inputs(std::numeric_limits<std::int64_t>::min(),
        std::numeric_limits<std::int64_t>::max(), at(2ms));
    EXPECT_CALL(sink, set_fault(FaultId::EngineSpeedSensorInvalid, false, at(3ms)));
    monitor.observe_engine_inputs(9000, -1, at(3ms));
    EXPECT_CALL(sink, set_fault(FaultId::VehicleSpeedSensorInvalid, false, at(4ms)));
    monitor.observe_engine_inputs(0, 300, at(4ms));
    monitor.stop();
}

TEST(DiagnosticMonitorTest, ValidTelemetryDoesNotClearAnInvalidRawSensorSample)
{
    DiagnosticStore store;
    DiagnosticMonitor monitor{store};
    monitor.start(at(0ms));
    monitor.observe_engine_inputs(9001, 0, at(0ms));
    publish_initial(monitor);
    expect_status(store, FaultId::EngineSpeedSensorInvalid, FaultStatus::Active);
    monitor.observe_engine_inputs(9000, 300, at(1ms));
    expect_status(store, FaultId::EngineSpeedSensorInvalid, FaultStatus::Cleared);
    monitor.stop();
}

TEST(DiagnosticMonitorTest, RecurringTimeoutCreatesANewEpisodeOnlyAfterRecovery)
{
    DiagnosticStore store;
    DiagnosticMonitor monitor{store, quick};
    monitor.start(at(0ms));
    publish_initial(monitor);
    monitor.on_tick(at(200ms));
    monitor.on_tick(at(201ms));
    monitor.on_message(msg::encode_engine({}, at(210ms)), at(210ms));
    monitor.on_tick(at(410ms));
    const auto record = store.record(FaultId::EngineCommunicationTimeout);
    ASSERT_TRUE(record);
    EXPECT_EQ(record->status, FaultStatus::Active);
    EXPECT_EQ(record->occurrence_count, 2U);
    EXPECT_EQ(record->first_occurrence, at(200ms));
    EXPECT_EQ(record->last_occurrence, at(410ms));
    EXPECT_EQ(record->last_recovery, at(210ms));
    monitor.stop();
}

TEST(DiagnosticMonitorTest, ThrowingSinkDoesNotCommitTheFailedTransition)
{
    StrictMock<MockSink> sink;
    DiagnosticMonitor monitor{sink, quick};
    monitor.start(at(0ms));
    EXPECT_CALL(sink, set_fault(FaultId::EngineMessageMissing, true, at(100ms)))
        .WillOnce(testing::Throw(std::runtime_error{"sink failed"}));
    EXPECT_THROW(monitor.on_tick(at(100ms)), std::runtime_error);
    EXPECT_CALL(sink, set_fault(FaultId::EngineMessageMissing, true, at(100ms)));
    EXPECT_CALL(sink, set_fault(FaultId::BrakeMessageMissing, true, at(100ms)));
    monitor.on_tick(at(100ms));
    monitor.on_tick(at(101ms));
    monitor.stop();
}

TEST(DiagnosticMonitorTest, PartialSinkFailureRetriesOnlyTheUncommittedFault)
{
    StrictMock<MockSink> sink;
    DiagnosticMonitor monitor{sink, quick};
    monitor.start(at(0ms));
    {
        testing::InSequence ordered;
        EXPECT_CALL(sink, set_fault(FaultId::EngineMessageMissing, true, at(100ms)));
        EXPECT_CALL(sink, set_fault(FaultId::BrakeMessageMissing, true, at(100ms)))
            .WillOnce(testing::Throw(std::runtime_error{"second fault rejected"}));
        EXPECT_CALL(sink, set_fault(FaultId::BrakeMessageMissing, true, at(100ms)));
        EXPECT_CALL(sink, set_fault(FaultId::EngineMessageMissing, false, at(110ms)));
    }
    EXPECT_THROW(monitor.on_tick(at(100ms)), std::runtime_error);
    EXPECT_THROW(monitor.on_tick(at(99ms)), std::invalid_argument);
    monitor.on_tick(at(100ms)); // Engine transition already committed; no repeat.
    monitor.on_tick(at(101ms));
    monitor.on_message(msg::encode_engine({}, at(110ms)), at(110ms));
    monitor.stop();
}

TEST(DiagnosticMonitorTest, RejectsNonPositiveDurations)
{
    StrictMock<MockSink> sink;
    EXPECT_THROW((void)(DiagnosticMonitor{sink, {0ms, 50ms, 200ms}}), std::invalid_argument);
    EXPECT_THROW((void)(DiagnosticMonitor{sink, {100ms, -1ms, 200ms}}), std::invalid_argument);
    EXPECT_THROW((void)(DiagnosticMonitor{sink, {100ms, 50ms, 0ms}}), std::invalid_argument);
}

TEST(DiagnosticMonitorTest, LifecycleAndTimeGuardsRejectCallsBeforeObservableWork)
{
    StrictMock<MockSink> sink;
    DiagnosticMonitor monitor{sink};
    const auto frame = msg::encode_engine({}, at(0ms));
    EXPECT_THROW(monitor.on_tick(at(0ms)), std::logic_error);
    EXPECT_THROW(monitor.on_message(frame, at(0ms)), std::logic_error);
    EXPECT_THROW(monitor.observe_engine_inputs(-1, -1, at(0ms)), std::logic_error);
    monitor.start(at(10ms));
    EXPECT_THROW(monitor.start(at(10ms)), std::logic_error);
    EXPECT_THROW(monitor.on_tick(at(9ms)), std::invalid_argument);
    EXPECT_THROW(monitor.on_message(frame, at(9ms)), std::invalid_argument);
    EXPECT_THROW(monitor.observe_engine_inputs(-1, -1, at(9ms)), std::invalid_argument);
    monitor.stop();
    monitor.stop();
    EXPECT_THROW(monitor.on_tick(at(11ms)), std::logic_error);
    EXPECT_THROW(monitor.on_message(frame, at(11ms)), std::logic_error);
    EXPECT_THROW(monitor.observe_engine_inputs(-1, -1, at(11ms)), std::logic_error);
    EXPECT_THROW(monitor.start(at(11ms)), std::logic_error);
}

TEST(DiagnosticMonitorTest, StoppingDoesNotInventRecoveryEvents)
{
    DiagnosticStore store;
    DiagnosticMonitor monitor{store};
    monitor.start(at(0ms));
    monitor.observe_engine_inputs(-1, 0, at(0ms));
    monitor.stop();
    expect_status(store, FaultId::EngineSpeedSensorInvalid, FaultStatus::Active);
    EXPECT_EQ(store.pending_events(), 1U);
    EXPECT_THROW(monitor.start(at(1ms)), std::logic_error);
}

TEST(DiagnosticMonitorTest, StopBeforeStartPreventsStarting)
{
    StrictMock<MockSink> sink;
    DiagnosticMonitor monitor{sink};
    monitor.stop();
    monitor.stop();
    EXPECT_THROW(monitor.start(at(0ms)), std::logic_error);
}

TEST(DiagnosticMonitorTest, ExtremeInjectedTimestampsDoNotOverflowDeadlineArithmetic)
{
    DiagnosticStore store;
    DiagnosticMonitor monitor{store};
    monitor.start(Timestamp::min());
    monitor.on_message(msg::encode_engine({}, Timestamp::min()), Timestamp::min());
    monitor.on_message(msg::encode_brake({}, Timestamp::min()), Timestamp::min());
    monitor.on_tick(Timestamp::max());
    expect_status(store, FaultId::EngineMessageStale, FaultStatus::Active);
    expect_status(store, FaultId::EngineCommunicationTimeout, FaultStatus::Active);
    monitor.on_message(msg::encode_engine({}, Timestamp::max()), Timestamp::max());
    monitor.on_message(msg::encode_brake({}, Timestamp::max()), Timestamp::max());
    expect_status(store, FaultId::EngineMessageStale, FaultStatus::Cleared);
    expect_status(store, FaultId::EngineCommunicationTimeout, FaultStatus::Cleared);
    expect_status(store, FaultId::BrakeMessageStale, FaultStatus::Cleared);
    expect_status(store, FaultId::BrakeCommunicationTimeout, FaultStatus::Cleared);
    monitor.stop();
}

} // namespace
