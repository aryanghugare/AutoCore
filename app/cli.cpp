#include "cli.hpp"

#include <iomanip>
#include <locale>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace vaecu::cli {
int run_concurrent_demo(std::ostream& output);

namespace {
constexpr std::string_view usage =
    "Usage: vaecu_sim [--scenario normal|sensor-failure|timeout|recovery] [--summary-only]\n"
    "       vaecu_sim --concurrent\n"
    "       vaecu_sim --help\n"
    "Default: normal. Scenarios use deterministic simulation time, with no sleeps.\n";

const char* scenario_name(DemoScenario scenario)
{
    switch (scenario) {
    case DemoScenario::Normal: return "normal";
    case DemoScenario::SensorFailure: return "sensor-failure";
    case DemoScenario::CommunicationTimeout: return "timeout";
    case DemoScenario::Recovery: return "recovery";
    }
    throw std::invalid_argument("Unknown demonstration scenario");
}

const char* availability(EcuAvailability value) { return value == EcuAvailability::Online ? "ONLINE" : "OFFLINE"; }
const char* health(SignalHealth value)
{
    switch (value) {
    case SignalHealth::Fresh: return "fresh";
    case SignalHealth::Stale: return "stale";
    case SignalHealth::Missing: return "missing";
    }
    return "unknown";
}

void timestamp(std::ostream& output, Timestamp now)
{
    output << '[' << std::setfill('0') << std::setw(4)
           << std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count()
           << " ms] " << std::setfill(' ');
}

void print_event(std::ostream& output, const DemoAction& event)
{
    timestamp(output, event.timestamp);
    switch (event.kind) {
    case DemoActionKind::Started: output << "RUN started"; break;
    case DemoActionKind::SensorFailed: output << "SCENARIO engine sensor invalid: rpm=-1; last valid input retained"; break;
    case DemoActionKind::EnginePaused: output << "SCENARIO Engine publication paused"; break;
    case DemoActionKind::SensorRecovered: output << "SCENARIO engine sensor restored: rpm=1500"; break;
    case DemoActionKind::EngineResumed: output << "SCENARIO Engine publication resumed"; break;
    case DemoActionKind::Stopped: output << "RUN stopped; resources released"; break;
    }
    output << '\n';
}

void print_event(std::ostream& output, const DemoTransmission& event)
{
    timestamp(output, event.frame.timestamp());
    const auto* source = event.source == DemoSource::Engine ? "Engine" :
        (event.source == DemoSource::Brake ? "Brake" : "Scenario");
    output << "CAN tx=" << source << " id=0x" << std::hex << std::uppercase
           << std::setfill('0') << std::setw(3) << event.frame.id().value()
           << std::dec << std::setfill(' ') << " len=" << event.frame.payload_size() << " data=[";
    for (std::size_t index = 0; index < event.frame.payload_size(); ++index) {
        if (index != 0) output << ' ';
        output << std::hex << std::setfill('0') << std::setw(2)
               << static_cast<unsigned>(event.frame.payload()[index]);
    }
    output << std::dec << std::setfill(' ') << "] routed=" << event.result.delivered
           << " dropped=" << event.result.dropped << '\n';
}

void print_event(std::ostream& output, const DiagnosticEvent& event)
{
    timestamp(output, event.timestamp);
    output << "DIAG code=0x" << std::hex << std::uppercase << std::setfill('0')
           << std::setw(4) << static_cast<unsigned>(event.id) << std::dec << std::setfill(' ')
           << (event.kind == DiagnosticEventKind::Raised ? " ACTIVE " : " RECOVERED ");
    for (const auto definition : fault_definitions) {
        if (definition.id == event.id) { output << definition.name; break; }
    }
    output << '\n';
}

void print_event(std::ostream& output, const DemoStatus& event)
{
    timestamp(output, event.timestamp);
    output << "ECU Engine=" << availability(event.engine) << " Brake=" << availability(event.brake)
           << " Dashboard=" << availability(event.dashboard);
    if (event.display.engine) {
        output << " | rpm=" << event.display.engine->engine_rpm
               << " speed=" << event.display.engine->vehicle_speed_kph << "km/h";
    }
    if (event.display.brake) output << " brake=" << (event.display.brake->applied ? "APPLIED" : "RELEASED");
    output << " | engine=" << health(event.display.engine_health)
           << " brake=" << health(event.display.brake_health) << '\n';
}
} // namespace

Options parse_arguments(const std::vector<std::string_view>& arguments)
{
    Options options;
    bool scenario_seen = false, summary_seen = false;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const auto argument = arguments[index];
        if (argument == "--help" || argument == "-h" || argument == "--concurrent") {
            if (arguments.size() != 1) throw std::invalid_argument("Help and concurrent mode must be used alone");
            options.mode = argument == "--concurrent" ? Mode::Concurrent : Mode::Help;
        } else if (argument == "--scenario") {
            if (scenario_seen) throw std::invalid_argument("--scenario may only be specified once");
            scenario_seen = true;
            if (++index == arguments.size()) throw std::invalid_argument("--scenario requires a name");
            const auto name = arguments[index];
            if (name == "normal") options.scenario = DemoScenario::Normal;
            else if (name == "sensor-failure") options.scenario = DemoScenario::SensorFailure;
            else if (name == "timeout") options.scenario = DemoScenario::CommunicationTimeout;
            else if (name == "recovery") options.scenario = DemoScenario::Recovery;
            else throw std::invalid_argument("Unknown scenario: " + std::string{name});
        } else if (argument == "--summary-only") {
            if (summary_seen) throw std::invalid_argument("--summary-only may only be specified once");
            summary_seen = options.summary_only = true;
        } else {
            throw std::invalid_argument("Unknown argument: " + std::string{argument});
        }
    }
    return options;
}

void print_report(const DemoReport& report, bool summary_only, std::ostream& output)
{
    // Local classic-locale formatting is repeatable and cannot leak hex/fill
    // flags into the caller's stream or depend on its formatting settings.
    std::ostringstream text;
    text.imbue(std::locale::classic());
    text << "Virtual Automotive ECU Platform | scenario=" << scenario_name(report.scenario) << " | deterministic\n"
         << "Software CAN simulation | time=relative ms | routing=FIFO\n";
    if (!summary_only) {
        for (const auto& event : report.events) {
            std::visit([&text](const auto& value) { print_event(text, value); }, event);
        }
    }
    const auto& summary = report.summary;
    text << "Summary: duration=" << summary.duration.count() << "ms CAN=" << summary.transmissions
         << " routed=" << summary.admitted_copies << " processed=" << summary.processed_copies
         << " dropped=" << summary.dropped_copies << '\n'
         << "Diagnostics: raised=" << summary.faults_raised << " recovered=" << summary.faults_recovered
         << " active=" << summary.active_faults << " lost=" << summary.discarded_events << '\n'
         << "Shutdown: all ECUs OFFLINE; pending_discarded=" << summary.pending_discarded << '\n';
    const auto rendered = text.str();
    output.write(rendered.data(), static_cast<std::streamsize>(rendered.size()));
    if (!output) throw std::runtime_error("Unable to write demonstration output");
}

int run(const std::vector<std::string_view>& arguments, std::ostream& output, std::ostream& error)
{
    Options options;
    try { options = parse_arguments(arguments); }
    catch (const std::invalid_argument& exception) {
        error << "Error: " << exception.what() << '\n' << usage;
        return 2;
    }
    try {
        int result = 0;
        if (options.mode == Mode::Help) {
            output.write(usage.data(), static_cast<std::streamsize>(usage.size()));
            if (!output) throw std::runtime_error("Unable to write help output");
        } else if (options.mode == Mode::Concurrent) {
            result = run_concurrent_demo(output);
        } else {
            print_report(run_demo_scenario(options.scenario), options.summary_only, output);
        }
        // Buffered destinations can accept writes but fail during synchronization.
        // Check that failure before returning success, after simulation cleanup.
        output.flush();
        if (!output) throw std::runtime_error("Unable to flush demonstration output");
        return result;
    } catch (const std::exception& exception) {
        error << "Simulation failed: " << exception.what() << '\n';
        return 1;
    }
}

} // namespace vaecu::cli
