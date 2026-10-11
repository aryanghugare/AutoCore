#include "vaecu/demo_scenario.hpp"
#include "vaecu/messages.hpp"

#include <gtest/gtest.h>

#include <array>
#include <stdexcept>
#include <type_traits>

namespace {
using namespace vaecu;
using namespace std::chrono_literals;

const DemoStatus* status_at(const DemoReport& report, std::chrono::milliseconds at)
{
    for (const auto& event : report.events) {
        const auto* status = std::get_if<DemoStatus>(&event);
        if (status && status->timestamp == Timestamp{at}) return status;
    }
    return nullptr;
}

TEST(DemoScenarioTest, FixedPlansHaveExactDurationsCountsAndBoundedTraces)
{
    struct Expected { DemoScenario scenario; std::chrono::milliseconds duration; std::size_t frames, raised, recovered, active; };
    const std::array<Expected, 4> cases{{
        {DemoScenario::Normal, 500ms, 8, 0, 0, 0},
        {DemoScenario::SensorFailure, 500ms, 8, 1, 0, 1},
        {DemoScenario::CommunicationTimeout, 1500ms, 10, 2, 0, 2},
        {DemoScenario::Recovery, 1500ms, 12, 3, 3, 0}
    }};
    for (const auto expected : cases) {
        SCOPED_TRACE(static_cast<int>(expected.scenario));
        const auto report = run_demo_scenario(expected.scenario);
        EXPECT_EQ(report.scenario, expected.scenario);
        EXPECT_EQ(report.summary.duration, expected.duration);
        EXPECT_EQ(report.summary.transmissions, expected.frames);
        EXPECT_EQ(report.summary.admitted_copies, expected.frames * 2);
        EXPECT_EQ(report.summary.processed_copies, expected.frames * 2);
        EXPECT_EQ(report.summary.dropped_copies, 0U);
        EXPECT_EQ(report.summary.faults_raised, expected.raised);
        EXPECT_EQ(report.summary.faults_recovered, expected.recovered);
        EXPECT_EQ(report.summary.active_faults, expected.active);
        EXPECT_EQ(report.summary.discarded_events, 0U);
        EXPECT_EQ(report.summary.pending_discarded, 0U);
        EXPECT_LE(report.events.size(), 64U);
        ASSERT_GE(report.events.size(), 3U);
        ASSERT_TRUE(std::holds_alternative<DemoAction>(report.events.front()));
        EXPECT_EQ(std::get<DemoAction>(report.events.front()).kind, DemoActionKind::Started);
        ASSERT_TRUE(std::holds_alternative<DemoAction>(report.events[report.events.size() - 2]));
        EXPECT_EQ(std::get<DemoAction>(report.events[report.events.size() - 2]).kind, DemoActionKind::Stopped);
        ASSERT_TRUE(std::holds_alternative<DemoStatus>(report.events.back()));
        const auto stopped = std::get<DemoStatus>(report.events.back());
        EXPECT_EQ(stopped.timestamp, Timestamp{expected.duration});
        EXPECT_EQ(stopped.engine, EcuAvailability::Offline);
        EXPECT_EQ(stopped.brake, EcuAvailability::Offline);
        EXPECT_EQ(stopped.dashboard, EcuAvailability::Offline);
        EXPECT_EQ(stopped.display.engine_health, SignalHealth::Stale);
    }
}

TEST(DemoScenarioTest, SensorFailureRetainsValidTelemetryAndDoesNotMarkHealthyCommunicationOffline)
{
    const auto report = run_demo_scenario(DemoScenario::SensorFailure);
    const auto* status = status_at(report, 500ms);
    ASSERT_NE(status, nullptr);
    ASSERT_TRUE(status->display.engine);
    EXPECT_EQ(status->engine, EcuAvailability::Online);
    EXPECT_EQ(status->display.engine_health, SignalHealth::Fresh);
    EXPECT_EQ(status->display.engine->engine_rpm, 1500);
    std::size_t faults = 0;
    for (const auto& event : report.events) {
        if (const auto* diagnostic = std::get_if<DiagnosticEvent>(&event)) {
            ++faults;
            EXPECT_EQ(diagnostic->id, FaultId::EngineSpeedSensorInvalid);
            EXPECT_EQ(diagnostic->kind, DiagnosticEventKind::Raised);
            EXPECT_EQ(diagnostic->timestamp, Timestamp{250ms});
        }
        if (const auto* transmission = std::get_if<DemoTransmission>(&event)) {
            if (transmission->source != DemoSource::Engine) continue;
            const auto telemetry = messages::decode_engine(transmission->frame);
            ASSERT_TRUE(telemetry);
            EXPECT_EQ(telemetry->engine_rpm, 1500);
        }
    }
    EXPECT_EQ(faults, 1U);
}

TEST(DemoScenarioTest, StaleDataAndOfflineCommunicationHaveDistinctBoundaries)
{
    const auto report = run_demo_scenario(DemoScenario::CommunicationTimeout);
    const auto* stale = status_at(report, 500ms);
    const auto* offline = status_at(report, 1000ms);
    ASSERT_NE(stale, nullptr);
    ASSERT_NE(offline, nullptr);
    EXPECT_EQ(stale->display.engine_health, SignalHealth::Stale);
    EXPECT_EQ(stale->engine, EcuAvailability::Online);
    EXPECT_EQ(offline->engine, EcuAvailability::Offline);
    EXPECT_EQ(offline->brake, EcuAvailability::Online);
    EXPECT_EQ(offline->dashboard, EcuAvailability::Online);
    std::size_t timeouts = 0;
    for (const auto& event : report.events) {
        if (const auto* diagnostic = std::get_if<DiagnosticEvent>(&event)) {
            if (diagnostic->id == FaultId::EngineCommunicationTimeout) {
                ++timeouts;
                EXPECT_EQ(diagnostic->kind, DiagnosticEventKind::Raised);
                EXPECT_EQ(diagnostic->timestamp, Timestamp{1000ms});
            }
        }
    }
    EXPECT_EQ(timeouts, 1U); // Shutdown does not clear the fault.
}

TEST(DemoScenarioTest, RecoveryClearsSensorStaleAndTimeoutFaultsAndRestoresOnlineState)
{
    const auto report = run_demo_scenario(DemoScenario::Recovery);
    const auto* recovered = status_at(report, 1250ms);
    ASSERT_NE(recovered, nullptr);
    EXPECT_EQ(recovered->engine, EcuAvailability::Online);
    EXPECT_EQ(recovered->display.engine_health, SignalHealth::Fresh);
    std::size_t recoveries = 0;
    for (const auto& event : report.events) {
        if (const auto* diagnostic = std::get_if<DiagnosticEvent>(&event)) {
            if (diagnostic->kind != DiagnosticEventKind::Recovered) continue;
            ++recoveries;
            EXPECT_EQ(diagnostic->timestamp, Timestamp{1250ms});
            EXPECT_TRUE(diagnostic->id == FaultId::EngineSpeedSensorInvalid ||
                        diagnostic->id == FaultId::EngineMessageStale ||
                        diagnostic->id == FaultId::EngineCommunicationTimeout);
        }
    }
    EXPECT_EQ(recoveries, 3U);
}

TEST(DemoScenarioTest, CommandsReachBrakeBeforeItsStatusReachesDashboard)
{
    const auto report = run_demo_scenario(DemoScenario::Normal);
    const auto* applied = status_at(report, 250ms);
    const auto* released = status_at(report, 500ms);
    ASSERT_NE(applied, nullptr);
    ASSERT_NE(released, nullptr);
    ASSERT_TRUE(applied->display.brake);
    ASSERT_TRUE(released->display.brake);
    EXPECT_TRUE(applied->display.brake->applied);
    EXPECT_FALSE(released->display.brake->applied);
    Timestamp last{};
    for (const auto& event : report.events) {
        const auto now = std::visit([](const auto& value) -> Timestamp {
            if constexpr (std::is_same_v<std::decay_t<decltype(value)>, DemoTransmission>) return value.frame.timestamp();
            else return value.timestamp;
        }, event);
        EXPECT_GE(now, last);
        last = now;
    }
}

TEST(DemoScenarioTest, InvalidScenarioIsRejectedAndLaterRunsHaveFreshState)
{
    EXPECT_THROW((void)run_demo_scenario(static_cast<DemoScenario>(-1)), std::invalid_argument);
    (void)run_demo_scenario(DemoScenario::SensorFailure);
    const auto normal = run_demo_scenario(DemoScenario::Normal);
    EXPECT_EQ(normal.summary.faults_raised, 0U);
    EXPECT_EQ(normal.summary.active_faults, 0U);
}

TEST(DemoScenarioTest, HooksObserveTheActualTraceAndDoNotChangeDefaultResults)
{
    std::vector<DemoEvent> observed;
    std::vector<std::chrono::milliseconds> cycles;
    const auto report = run_demo_scenario(DemoScenario::Recovery, {
        [&](auto elapsed) { cycles.push_back(elapsed); return true; },
        [&](const DemoEvent& event) { observed.push_back(event); }
    });
    ASSERT_EQ(observed.size(), report.events.size());
    for (std::size_t index = 0; index < observed.size(); ++index) {
        EXPECT_EQ(observed[index].index(), report.events[index].index());
    }
    ASSERT_EQ(cycles.size(), 7U);
    EXPECT_EQ(cycles.front(), 0ms);
    EXPECT_EQ(cycles.back(), 1500ms);
    EXPECT_EQ(report.summary.transmissions, 12U);
    EXPECT_EQ(report.summary.faults_recovered, 3U);
    EXPECT_EQ(std::get<DemoStatus>(observed.back()).engine, EcuAvailability::Offline);
}

TEST(DemoScenarioTest, CancellationStopsBeforeNextCycleWithoutClearingFaults)
{
    const auto report = run_demo_scenario(DemoScenario::SensorFailure,
        {[](auto elapsed) { return elapsed <= 250ms; }, {}});
    EXPECT_EQ(report.summary.duration, 250ms);
    EXPECT_EQ(report.summary.transmissions, 5U);
    EXPECT_EQ(report.summary.processed_copies, 10U);
    EXPECT_EQ(report.summary.active_faults, 1U);
    EXPECT_EQ(report.summary.faults_recovered, 0U);
    EXPECT_EQ(report.summary.pending_discarded, 0U);
    EXPECT_EQ(std::get<DemoStatus>(report.events.back()).engine, EcuAvailability::Offline);
    const auto empty = run_demo_scenario(DemoScenario::Normal, {[](auto) { return false; }, {}});
    EXPECT_EQ(empty.summary.transmissions, 0U);
    EXPECT_EQ(empty.summary.duration, 0ms);
}

TEST(DemoScenarioTest, ObserverFailurePropagatesAndLaterSessionsRemainIndependent)
{
    EXPECT_THROW((void)run_demo_scenario(DemoScenario::Normal, {{}, [](const auto&) {
        throw std::runtime_error("observer failed");
    }}), std::runtime_error);
    const auto report = run_demo_scenario(DemoScenario::Normal);
    EXPECT_EQ(report.summary.transmissions, 8U);
    EXPECT_EQ(report.summary.active_faults, 0U);
}

} // namespace
