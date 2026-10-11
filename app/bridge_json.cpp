#include "bridge_json.hpp"

#include <iomanip>
#include <iterator>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace vaecu::bridge {
namespace {
auto milliseconds(Timestamp value)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(value.time_since_epoch()).count();
}
const char* health(SignalHealth value)
{
    switch (value) {
    case SignalHealth::Fresh: return "fresh";
    case SignalHealth::Stale: return "stale";
    case SignalHealth::Missing: return "missing";
    }
    throw std::invalid_argument("Unknown signal health");
}
const char* online(EcuAvailability value) { return value == EcuAvailability::Online ? "online" : "offline"; }
void encode(std::ostream& out, const DemoAction& event)
{
    constexpr const char* actions[]{"started", "sensor-failed", "engine-paused", "sensor-recovered", "engine-resumed", "stopped"};
    const auto index = static_cast<std::size_t>(event.kind);
    if (index >= std::size(actions)) throw std::invalid_argument("Unknown action");
    out << "\"type\":\"action\",\"timeMs\":" << milliseconds(event.timestamp)
        << ",\"action\":\"" << actions[index] << '"';
}
void encode(std::ostream& out, const DemoTransmission& event)
{
    const char* source = event.source == DemoSource::Engine ? "Engine" :
        (event.source == DemoSource::Brake ? "Brake" : "Scenario");
    out << "\"type\":\"can\",\"timeMs\":" << milliseconds(event.frame.timestamp())
        << ",\"source\":\"" << source << "\",\"id\":" << event.frame.id().value() << ",\"payload\":[";
    for (std::size_t index = 0; index < event.frame.payload_size(); ++index) {
        if (index) out << ',';
        out << static_cast<unsigned>(event.frame.payload()[index]);
    }
    out << "],\"routed\":" << event.result.delivered << ",\"dropped\":" << event.result.dropped;
}
void encode(std::ostream& out, const DiagnosticEvent& event)
{
    out << "\"type\":\"diagnostic\",\"timeMs\":" << milliseconds(event.timestamp)
        << ",\"code\":" << static_cast<unsigned>(event.id)
        << ",\"status\":\"" << (event.kind == DiagnosticEventKind::Raised ? "active" : "cleared") << "\",\"name\":";
    for (const auto definition : fault_definitions) {
        if (definition.id == event.id) { out << std::quoted(std::string{definition.name}); return; }
    }
    throw std::invalid_argument("Unknown diagnostic code");
}
void encode(std::ostream& out, const DemoStatus& event)
{
    out << "\"type\":\"status\",\"timeMs\":" << milliseconds(event.timestamp)
        << ",\"ecus\":{\"Engine\":\"" << online(event.engine) << "\",\"Brake\":\""
        << online(event.brake) << "\",\"Dashboard\":\"" << online(event.dashboard) << "\"}"
        << ",\"engineHealth\":\"" << health(event.display.engine_health)
        << "\",\"brakeHealth\":\"" << health(event.display.brake_health) << "\",\"rpm\":";
    if (event.display.engine) out << event.display.engine->engine_rpm; else out << "null";
    out << ",\"speedKph\":";
    if (event.display.engine) out << event.display.engine->vehicle_speed_kph; else out << "null";
    out << ",\"brakeApplied\":";
    if (event.display.brake) out << (event.display.brake->applied ? "true" : "false"); else out << "null";
}
} // namespace

DemoScenario parse_scenario(const std::string& name)
{
    if (name == "normal") return DemoScenario::Normal;
    if (name == "sensor-failure") return DemoScenario::SensorFailure;
    if (name == "timeout") return DemoScenario::CommunicationTimeout;
    if (name == "recovery") return DemoScenario::Recovery;
    throw std::invalid_argument("Unknown scenario");
}

std::string event_json(const DemoEvent& event)
{
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << "{\"version\":1,";
    std::visit([&](const auto& value) { encode(out, value); }, event);
    out << '}';
    return out.str();
}

std::string summary_json(const DemoSummary& summary, bool cancelled)
{
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << "{\"version\":1,\"type\":\"summary\",\"timeMs\":" << summary.duration.count()
        << ",\"cancelled\":" << (cancelled ? "true" : "false")
        << ",\"transmissions\":" << summary.transmissions << ",\"routed\":" << summary.admitted_copies
        << ",\"processed\":" << summary.processed_copies << ",\"dropped\":" << summary.dropped_copies
        << ",\"raised\":" << summary.faults_raised << ",\"recovered\":" << summary.faults_recovered
        << ",\"active\":" << summary.active_faults << ",\"lost\":" << summary.discarded_events
        << ",\"pendingDiscarded\":" << summary.pending_discarded << '}';
    return out.str();
}
} // namespace vaecu::bridge
