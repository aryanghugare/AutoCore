#include "vaecu/concurrent_simulation.hpp"

#include <gtest/gtest.h>

#include <array>
#include <future>
#include <limits>
#include <vector>

namespace {
using namespace vaecu;
using namespace std::chrono_literals;
namespace msg = vaecu::messages;

CanFrame frame(std::size_t sequence)
{
    return {msg::engine_telemetry_id, {}, 0,
        Timestamp{Timestamp::duration{static_cast<Timestamp::duration::rep>(sequence)}}};
}

template<class Predicate>
std::optional<TelemetrySnapshot> await_state(ConcurrentSimulation& simulation, Predicate predicate)
{
    auto snapshot = simulation.dashboard_snapshot();
    const auto deadline = Timestamp::clock::now() + 2s;
    while (!predicate(snapshot.state)) {
        const auto next = simulation.wait_for_dashboard(snapshot.revision, deadline);
        if (!next) return std::nullopt;
        snapshot = *next;
    }
    return snapshot;
}

bool await_event(ConcurrentSimulation& simulation, FaultId id, DiagnosticEventKind kind)
{
    const auto deadline = Timestamp::clock::now() + 2s;
    for (;;) {
        const auto event = simulation.wait_event_until(deadline);
        if (!event) return false;
        if (event->id == id && event->kind == kind) return true;
    }
}

TEST(ConcurrentQueueTest, ConcurrentProducersRespectCapacityAndRejectNewest)
{
    VirtualCanBus bus;
    const auto inbox = bus.register_receiver("small", {msg::engine_telemetry_id}, 8);
    std::array<std::size_t, 2> accepted{}, dropped{};
    std::array<std::thread, 2> producers;
    for (std::size_t worker = 0; worker < producers.size(); ++worker) {
        producers[worker] = std::thread{[&, worker] {
            for (std::size_t sequence = 0; sequence < 1000; ++sequence) {
                const auto sent = bus.send(frame(worker * 1000 + sequence));
                accepted[worker] += sent.delivered;
                dropped[worker] += sent.dropped;
            }
        }};
    }
    for (auto& producer : producers) producer.join();
    EXPECT_EQ(accepted[0] + accepted[1], 8U);
    EXPECT_EQ(dropped[0] + dropped[1], 1992U);
    EXPECT_EQ(inbox->pending(), inbox->capacity());
    for (std::size_t index = 0; index < 8; ++index) {
        EXPECT_TRUE(std::holds_alternative<CanFrame>(inbox->try_receive()));
    }
    EXPECT_EQ(std::get<ReceiveStatus>(inbox->try_receive()), ReceiveStatus::Empty);
}

TEST(ConcurrentQueueTest, FanoutHasTheSameSerializedOrderForConcurrentProducers)
{
    VirtualCanBus bus;
    const auto first = bus.register_receiver("first", {msg::engine_telemetry_id}, 800);
    const auto second = bus.register_receiver("second", {msg::engine_telemetry_id}, 800);
    std::array<std::thread, 4> producers;
    std::atomic<bool> admission_ok{true};
    for (std::size_t worker = 0; worker < producers.size(); ++worker) {
        producers[worker] = std::thread{[&, worker] {
            for (std::size_t sequence = 0; sequence < 200; ++sequence) {
                const auto sent = bus.send(frame(worker * 200 + sequence));
                if (sent.delivered != 2 || sent.dropped != 0) admission_ok = false;
            }
        }};
    }
    for (auto& producer : producers) producer.join();
    ASSERT_TRUE(admission_ok);
    std::array<std::int64_t, 4> last{-1, -1, -1, -1};
    for (std::size_t count = 0; count < 800; ++count) {
        const auto a = first->try_receive();
        const auto b = second->try_receive();
        ASSERT_TRUE(std::holds_alternative<CanFrame>(a));
        ASSERT_TRUE(std::holds_alternative<CanFrame>(b));
        const auto timestamp = std::get<CanFrame>(a).timestamp();
        EXPECT_EQ(timestamp, std::get<CanFrame>(b).timestamp());
        const auto sequence = timestamp.time_since_epoch().count();
        const auto worker = static_cast<std::size_t>(sequence / 200);
        ASSERT_LT(worker, last.size());
        EXPECT_EQ(sequence % 200, last[worker] + 1);
        last[worker] = sequence % 200;
    }
    for (const auto value : last) EXPECT_EQ(value, 199);
}

TEST(ConcurrentQueueTest, WaitingConsumerReceivesAnOwnedFrame)
{
    VirtualCanBus bus;
    const auto inbox = bus.register_receiver("waiting", {msg::engine_telemetry_id}, 1);
    std::promise<void> entered;
    auto consumer = std::async(std::launch::async, [&] {
        entered.set_value();
        return inbox->receive_until(Timestamp::clock::now() + 2s);
    });
    entered.get_future().wait();
    auto original = frame(42);
    EXPECT_EQ(bus.send(original).delivered, 1U);
    original = frame(99);
    ASSERT_EQ(consumer.wait_for(1s), std::future_status::ready);
    const auto result = consumer.get();
    ASSERT_TRUE(std::holds_alternative<CanFrame>(result));
    EXPECT_EQ(std::get<CanFrame>(result).timestamp(), frame(42).timestamp());
}

TEST(ConcurrentQueueTest, ExpiredWaitReturnsEmptyButAvailableDataWins)
{
    VirtualCanBus bus;
    const auto inbox = bus.register_receiver("deadline", {msg::engine_telemetry_id}, 1);
    EXPECT_EQ(std::get<ReceiveStatus>(inbox->receive_until(Timestamp::min())), ReceiveStatus::Empty);
    EXPECT_EQ(bus.send(frame(1)).delivered, 1U);
    EXPECT_TRUE(std::holds_alternative<CanFrame>(inbox->receive_until(Timestamp::min())));
}

TEST(ConcurrentQueueTest, TimedWaitDoesNotReturnEarlyWithoutData)
{
    CanReceiver inbox{{}, 1};
    const auto deadline = Timestamp::clock::now() + 20ms;
    EXPECT_EQ(std::get<ReceiveStatus>(inbox.receive_until(deadline)), ReceiveStatus::Empty);
    EXPECT_GE(Timestamp::clock::now(), deadline);
}

TEST(ConcurrentQueueTest, CloseWakesAllConsumersAndDiscardsPendingData)
{
    VirtualCanBus bus;
    const auto inbox = bus.register_receiver("waiting", {msg::engine_telemetry_id}, 2);
    std::array<std::future<ReceiveResult>, 4> consumers;
    for (auto& consumer : consumers) {
        consumer = std::async(std::launch::async, [&] {
            return inbox->receive_until(Timestamp::clock::now() + 2s);
        });
    }
    EXPECT_EQ(bus.close(), 0U);
    for (auto& consumer : consumers) {
        ASSERT_EQ(consumer.wait_for(1s), std::future_status::ready);
        EXPECT_EQ(std::get<ReceiveStatus>(consumer.get()), ReceiveStatus::Closed);
    }
    VirtualCanBus pending_bus;
    const auto pending = pending_bus.register_receiver("pending", {msg::engine_telemetry_id}, 2);
    EXPECT_EQ(pending_bus.send(frame(1)).delivered, 1U);
    EXPECT_EQ(pending_bus.send(frame(2)).delivered, 1U);
    EXPECT_EQ(pending_bus.close(), 2U);
    EXPECT_EQ(pending->pending(), 0U);
    EXPECT_EQ(std::get<ReceiveStatus>(pending->receive_until(Timestamp::max())), ReceiveStatus::Closed);
}

TEST(ConcurrentQueueTest, EveryAdmittedCopyIsConsumedOrDiscardedAtShutdown)
{
    VirtualCanBus bus;
    const auto inbox = bus.register_receiver("consumer", {msg::engine_telemetry_id}, 8);
    std::array<std::size_t, 3> accepted{}, dropped{};
    auto consumer = std::async(std::launch::async, [&] {
        std::vector<bool> seen(300, false);
        std::size_t count = 0;
        bool valid = true;
        for (;;) {
            const auto result = inbox->receive_until(Timestamp::clock::now() + 2s);
            if (const auto* value = std::get_if<CanFrame>(&result)) {
                const auto sequence = static_cast<std::size_t>(value->timestamp().time_since_epoch().count());
                if (sequence >= seen.size() || seen[sequence]) valid = false;
                else seen[sequence] = true;
                ++count;
            } else {
                return std::make_pair(count, valid && std::get<ReceiveStatus>(result) == ReceiveStatus::Closed);
            }
        }
    });
    std::array<std::thread, 3> producers;
    for (std::size_t worker = 0; worker < producers.size(); ++worker) {
        producers[worker] = std::thread{[&, worker] {
            for (std::size_t sequence = 0; sequence < 100; ++sequence) {
                const auto sent = bus.send(frame(worker * 100 + sequence));
                accepted[worker] += sent.delivered;
                dropped[worker] += sent.dropped;
            }
        }};
    }
    for (auto& producer : producers) producer.join();
    const auto discarded = bus.close();
    const auto result = consumer.get();
    EXPECT_TRUE(result.second);
    EXPECT_EQ(accepted[0] + accepted[1] + accepted[2], result.first + discarded);
    EXPECT_EQ(accepted[0] + accepted[1] + accepted[2] + dropped[0] + dropped[1] + dropped[2], 300U);
}

TEST(ConcurrentQueueTest, SendAndReceiverClosureAreSafeConcurrently)
{
    VirtualCanBus bus;
    const auto inbox = bus.register_receiver("closing", {msg::engine_telemetry_id}, 8);
    std::atomic<bool> valid{true};
    std::thread producer{[&] {
        for (std::size_t sequence = 0; sequence < 2000; ++sequence) {
            const auto sent = bus.send(frame(sequence));
            if (sent.status != SendStatus::Routed || sent.delivered + sent.dropped > 1) valid = false;
        }
    }};
    (void)inbox->close();
    producer.join();
    EXPECT_TRUE(valid);
    EXPECT_EQ(inbox->pending(), 0U);
    const auto sent = bus.send(frame(0));
    EXPECT_EQ(sent.delivered, 0U);
    EXPECT_EQ(sent.dropped, 0U);
}

TEST(ConcurrentDiagnosticTest, ReporterCanConsumeEventsAndSnapshotsWhileFaultsChange)
{
    DiagnosticStore store{4};
    auto reporter = std::async(std::launch::async, [&] {
        std::size_t consumed = 0;
        bool valid = true;
        while (const auto event = store.wait_event_until(Timestamp::clock::now() + 2s)) {
            ++consumed;
            const auto record = store.record(event->id);
            valid = valid && record.has_value() && record->occurrence_count >= 1 &&
                record->first_occurrence <= record->last_occurrence;
            (void)store.pending_events();
            (void)store.discarded_events();
        }
        return std::make_pair(consumed, valid && store.events_closed());
    });
    std::thread producer{[&] {
        for (std::int64_t episode = 0; episode < 1000; ++episode) {
            store.set_fault(FaultId::EnginePayloadInvalid, true, Timestamp{std::chrono::milliseconds{episode * 2}});
            store.set_fault(FaultId::EnginePayloadInvalid, false, Timestamp{std::chrono::milliseconds{episode * 2 + 1}});
        }
        store.close_events();
    }};
    producer.join();
    const auto reported = reporter.get();
    EXPECT_TRUE(reported.second);
    EXPECT_EQ(reported.first + store.discarded_events(), 2000U);
    const auto record = store.record(FaultId::EnginePayloadInvalid);
    ASSERT_TRUE(record);
    EXPECT_EQ(record->status, FaultStatus::Cleared);
    EXPECT_EQ(record->occurrence_count, 1000U);
    EXPECT_EQ(record->last_recovery, Timestamp{1999ms});
}

TEST(ConcurrentDiagnosticTest, ClosedFeedRetainsEventsAndRecordsButRejectsWrites)
{
    DiagnosticStore store;
    EXPECT_FALSE(store.wait_event_until(Timestamp::min()));
    store.set_fault(FaultId::EnginePayloadInvalid, true, Timestamp{});
    store.set_fault(FaultId::EnginePayloadInvalid, false, Timestamp{});
    store.close_events();
    store.close_events();
    const auto raised = store.wait_event_until(Timestamp::max());
    const auto recovered = store.wait_event_until(Timestamp::max());
    ASSERT_TRUE(raised);
    ASSERT_TRUE(recovered);
    EXPECT_EQ(raised->kind, DiagnosticEventKind::Raised);
    EXPECT_EQ(recovered->kind, DiagnosticEventKind::Recovered);
    EXPECT_FALSE(store.wait_event_until(Timestamp::max()));
    const auto record = store.record(FaultId::EnginePayloadInvalid);
    ASSERT_TRUE(record);
    EXPECT_EQ(record->status, FaultStatus::Cleared);
    EXPECT_THROW(store.set_fault(FaultId::EnginePayloadInvalid, false, Timestamp{}), std::logic_error);
}

TEST(ConcurrentDiagnosticTest, CloseWakesAnEmptyEventFeed)
{
    DiagnosticStore store;
    auto waiter = std::async(std::launch::async, [&] {
        return store.wait_event_until(Timestamp::clock::now() + 2s);
    });
    store.close_events();
    ASSERT_EQ(waiter.wait_for(1s), std::future_status::ready);
    EXPECT_FALSE(waiter.get());
}

TEST(ConcurrentSimulationTest, WorkersPublishTelemetryAndConsumeBrakeEvents)
{
    ConcurrentSimulation simulation;
    simulation.start();
    const auto initial = await_state(simulation, [](const DashboardState& state) {
        return state.engine_health == SignalHealth::Fresh && state.brake_health == SignalHealth::Fresh;
    });
    ASSERT_TRUE(initial);
    EXPECT_EQ(initial->state.engine->engine_rpm, 1500);
    const auto sent = simulation.send(msg::encode_brake_event(msg::BrakeEvent::Apply, Timestamp::clock::now()));
    EXPECT_EQ(sent.status, SendStatus::Routed);
    EXPECT_EQ(sent.delivered, 2U);
    ASSERT_TRUE(await_state(simulation, [](const DashboardState& state) {
        return state.brake && state.brake->applied;
    }));
    simulation.stop();
    EXPECT_NO_THROW(simulation.rethrow_if_failed());
    EXPECT_EQ(simulation.dashboard_snapshot().state.engine_health, SignalHealth::Stale);
    EXPECT_EQ(simulation.dashboard_snapshot().state.brake_health, SignalHealth::Stale);
}

TEST(ConcurrentSimulationTest, SilentEngineTimesOutAndRecoversWhileOtherWorkersRun)
{
    ConcurrentConfig config;
    config.tick_period = 5ms;
    config.diagnostics = {100ms, 30ms, 80ms};
    config.dashboard = {30ms, 30ms};
    ConcurrentSimulation simulation{config};
    simulation.start();
    ASSERT_TRUE(await_state(simulation, [](const DashboardState& state) {
        return state.engine && state.brake;
    }));
    simulation.set_engine_publishing(false);
    ASSERT_TRUE(await_event(simulation, FaultId::EngineCommunicationTimeout, DiagnosticEventKind::Raised));
    const auto active = simulation.fault(FaultId::EngineCommunicationTimeout);
    ASSERT_TRUE(active);
    EXPECT_EQ(active->status, FaultStatus::Active);
    simulation.set_engine_publishing(true);
    ASSERT_TRUE(await_event(simulation, FaultId::EngineCommunicationTimeout, DiagnosticEventKind::Recovered));
    const auto recovered = simulation.fault(FaultId::EngineCommunicationTimeout);
    ASSERT_TRUE(recovered);
    EXPECT_EQ(recovered->status, FaultStatus::Cleared);
    EXPECT_EQ(recovered->occurrence_count, 1U);
    ASSERT_TRUE(recovered->last_recovery);
    EXPECT_GE(*recovered->last_recovery, recovered->last_occurrence);
    simulation.stop();
    EXPECT_NO_THROW(simulation.rethrow_if_failed());
}

TEST(ConcurrentSimulationTest, MissingEngineIsDetectedDuringStartupSilence)
{
    ConcurrentConfig config;
    config.publish_engine = false;
    config.tick_period = 5ms;
    config.diagnostics = {30ms, 30ms, 80ms};
    ConcurrentSimulation simulation{config};
    simulation.start();
    ASSERT_TRUE(await_event(simulation, FaultId::EngineMessageMissing, DiagnosticEventKind::Raised));
    EXPECT_FALSE(simulation.fault(FaultId::EngineMessageStale));
    ASSERT_TRUE(await_event(simulation, FaultId::EngineCommunicationTimeout, DiagnosticEventKind::Raised));
    simulation.set_engine_publishing(true);
    ASSERT_TRUE(await_event(simulation, FaultId::EngineCommunicationTimeout, DiagnosticEventKind::Recovered));
    const auto missing = simulation.fault(FaultId::EngineMessageMissing);
    ASSERT_TRUE(missing);
    EXPECT_EQ(missing->status, FaultStatus::Cleared);
    simulation.stop();
    EXPECT_NO_THROW(simulation.rethrow_if_failed());
}

TEST(ConcurrentSimulationTest, StopInterruptsLongWaitsAndConcurrentStopsJoinSafely)
{
    ConcurrentConfig config;
    config.tick_period = 60000ms;
    ConcurrentSimulation simulation{config};
    simulation.start();
    ASSERT_TRUE(await_state(simulation, [](const DashboardState& state) { return state.engine && state.brake; }));
    auto first = std::async(std::launch::async, [&] { simulation.stop(); });
    auto second = std::async(std::launch::async, [&] { simulation.stop(); });
    ASSERT_EQ(first.wait_for(2s), std::future_status::ready);
    ASSERT_EQ(second.wait_for(2s), std::future_status::ready);
    first.get();
    second.get();
    EXPECT_FALSE(simulation.wait_for_dashboard(simulation.dashboard_snapshot().revision, Timestamp::max()));
    EXPECT_FALSE(simulation.wait_event_until(Timestamp::max()));
    EXPECT_NO_THROW(simulation.rethrow_if_failed());
}

TEST(ConcurrentSimulationTest, StartStopContractsRejectLateControlAndTransmission)
{
    ConcurrentSimulation simulation;
    EXPECT_EQ(simulation.send(frame(0)).status, SendStatus::Closed);
    EXPECT_THROW(simulation.set_engine_publishing(false), std::logic_error);
    simulation.start();
    EXPECT_THROW(simulation.start(), std::logic_error);
    simulation.request_stop();
    simulation.stop();
    simulation.stop();
    EXPECT_EQ(simulation.send(frame(0)).status, SendStatus::Closed);
    EXPECT_THROW(simulation.start(), std::logic_error);
    EXPECT_THROW(simulation.set_brake_publishing(true), std::logic_error);
}

TEST(ConcurrentSimulationTest, StopBeforeStartIsPermanentAndClosesReporting)
{
    ConcurrentSimulation simulation;
    simulation.stop();
    EXPECT_THROW(simulation.start(), std::logic_error);
    EXPECT_FALSE(simulation.wait_event_until(Timestamp::max()));
    EXPECT_FALSE(simulation.wait_for_dashboard(0, Timestamp::max()));
}

TEST(ConcurrentSimulationTest, CancellationBeforeStartWakesDiagnosticReadersWithoutJoining)
{
    ConcurrentSimulation simulation;
    std::promise<void> entered;
    auto reader = std::async(std::launch::async, [&] {
        entered.set_value();
        return simulation.wait_event_until(Timestamp::clock::now() + 5s);
    });
    entered.get_future().wait();
    simulation.request_stop();
    const auto status = reader.wait_for(200ms);
    // Clean up even on regression, so a failed assertion cannot strand the
    // reader in a future's blocking destructor.
    simulation.stop();
    EXPECT_EQ(status, std::future_status::ready);
    EXPECT_FALSE(reader.get());
    EXPECT_THROW(simulation.start(), std::logic_error);
    EXPECT_NO_THROW(simulation.rethrow_if_failed());
}

TEST(ConcurrentSimulationTest, ReportingRemainsSafeWhileWorkersStop)
{
    ConcurrentSimulation simulation;
    simulation.start();
    std::array<std::future<std::size_t>, 3> readers;
    for (auto& reader : readers) {
        reader = std::async(std::launch::async, [&] {
            std::size_t observed = 0;
            std::uint64_t revision = 0;
            while (const auto snapshot = simulation.wait_for_dashboard(revision, Timestamp::clock::now() + 2s)) {
                revision = snapshot->revision;
                ++observed;
                (void)simulation.fault(FaultId::EngineMessageStale);
                (void)simulation.discarded_events();
            }
            return observed;
        });
    }
    const auto initial = await_state(simulation, [](const DashboardState& state) {
        return state.engine && state.brake;
    });
    // Stop before a fatal assertion: futures are destroyed before simulation,
    // and their readers otherwise keep waiting for updates from live workers.
    simulation.stop();
    ASSERT_TRUE(initial);
    for (auto& reader : readers) {
        ASSERT_EQ(reader.wait_for(1s), std::future_status::ready);
        (void)reader.get();
    }
    EXPECT_NO_THROW(simulation.rethrow_if_failed());
}

TEST(ConcurrentSimulationTest, DestructionJoinsWorkersAcrossRepeatedSessions)
{
    for (int session = 0; session < 10; ++session) {
        ConcurrentSimulation simulation;
        simulation.start();
        ASSERT_TRUE(await_state(simulation, [](const DashboardState& state) { return state.engine && state.brake; }));
        EXPECT_NO_THROW(simulation.rethrow_if_failed());
        // Destructor cancels and joins; no explicit stop on this path.
    }
}

TEST(ConcurrentSimulationTest, CancellationRacingStartupAlwaysLeavesJoinedWorkers)
{
    for (int session = 0; session < 10; ++session) {
        ConcurrentSimulation simulation;
        auto startup = std::async(std::launch::async, [&] {
            try { simulation.start(); }
            catch (const std::logic_error& error) {
                EXPECT_STREQ(error.what(), "Concurrent simulation can only start once");
            }
            catch (const std::runtime_error& error) {
                EXPECT_STREQ(error.what(), "Concurrent startup cancelled");
            }
        });
        simulation.request_stop();
        ASSERT_EQ(startup.wait_for(2s), std::future_status::ready);
        startup.get();
        simulation.stop();
        EXPECT_EQ(simulation.send(frame(0)).status, SendStatus::Closed);
        EXPECT_NO_THROW(simulation.rethrow_if_failed());
    }
}

TEST(ConcurrentSimulationTest, RejectsInvalidConfigurationWithoutStartingThreads)
{
    ConcurrentConfig config;
    config.tick_period = 0ms;
    EXPECT_THROW((void)ConcurrentSimulation{config}, std::invalid_argument);
    config.tick_period = std::chrono::milliseconds{std::numeric_limits<std::int64_t>::max()};
    EXPECT_THROW((void)ConcurrentSimulation{config}, std::invalid_argument);
    config.tick_period = 10ms;
    config.inbox_capacity = 0;
    EXPECT_THROW((void)ConcurrentSimulation{config}, std::invalid_argument);
    config.inbox_capacity = 64;
    config.event_capacity = 0;
    EXPECT_THROW((void)ConcurrentSimulation{config}, std::invalid_argument);
}

} // namespace
