#include "bridge_json.hpp"
#include "vaecu/messages.hpp"
#include <gtest/gtest.h>

namespace {
using namespace vaecu;
using namespace std::chrono_literals;
TEST(BridgeJsonTest, EncodesActualBytesAndAdmissionCounts)
{
    const DemoEvent event = DemoTransmission{DemoSource::Engine,
        messages::encode_engine({1500, 60}, Timestamp{250ms}), {SendStatus::Routed, 2, 0}};
    EXPECT_EQ(bridge::event_json(event),
        "{\"version\":1,\"type\":\"can\",\"timeMs\":250,\"source\":\"Engine\",\"id\":256,\"payload\":[220,5,60,0],\"routed\":2,\"dropped\":0}");
}
TEST(BridgeJsonTest, MissingValuesAreNullAndHealthIsExplicit)
{
    const DemoEvent event = DemoStatus{Timestamp{}, EcuAvailability::Offline,
        EcuAvailability::Offline, EcuAvailability::Online, {}};
    EXPECT_EQ(bridge::event_json(event),
        "{\"version\":1,\"type\":\"status\",\"timeMs\":0,\"ecus\":{\"Engine\":\"offline\",\"Brake\":\"offline\",\"Dashboard\":\"online\"},\"engineHealth\":\"missing\",\"brakeHealth\":\"missing\",\"rpm\":null,\"speedKph\":null,\"brakeApplied\":null}");
}
TEST(BridgeJsonTest, DiagnosticCatalogProducesNamedRecoveryEvents)
{
    EXPECT_EQ(bridge::event_json(DiagnosticEvent{FaultId::EngineCommunicationTimeout,
        DiagnosticEventKind::Recovered, Timestamp{1250ms}}),
        "{\"version\":1,\"type\":\"diagnostic\",\"timeMs\":1250,\"code\":4356,\"status\":\"cleared\",\"name\":\"engine communication timeout\"}");
}
TEST(BridgeJsonTest, SummaryDistinguishesCancellationFromRecovery)
{
    DemoSummary summary;
    summary.duration = 250ms;
    summary.active_faults = 1;
    EXPECT_EQ(bridge::summary_json(summary, true),
        "{\"version\":1,\"type\":\"summary\",\"timeMs\":250,\"cancelled\":true,\"transmissions\":0,\"routed\":0,\"processed\":0,\"dropped\":0,\"raised\":0,\"recovered\":0,\"active\":1,\"lost\":0,\"pendingDiscarded\":0}");
}
TEST(BridgeJsonTest, OnlyDefinedScenarioNamesAreAccepted)
{
    EXPECT_EQ(bridge::parse_scenario("normal"), DemoScenario::Normal);
    EXPECT_EQ(bridge::parse_scenario("sensor-failure"), DemoScenario::SensorFailure);
    EXPECT_EQ(bridge::parse_scenario("timeout"), DemoScenario::CommunicationTimeout);
    EXPECT_EQ(bridge::parse_scenario("recovery"), DemoScenario::Recovery);
    EXPECT_THROW((void)bridge::parse_scenario("../other"), std::invalid_argument);
}
} // namespace
