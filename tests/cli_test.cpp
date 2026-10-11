#include "cli.hpp"

#include <gtest/gtest.h>

#include <array>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace {
using namespace vaecu;
namespace cli = vaecu::cli;

TEST(CliTest, DefaultsAndScenarioNamesAreExplicit)
{
    EXPECT_EQ(cli::parse_arguments({}).scenario, DemoScenario::Normal);
    EXPECT_EQ(cli::parse_arguments({}).mode, cli::Mode::Scenario);
    const std::array<std::pair<std::string_view, DemoScenario>, 4> scenarios{{
        {"normal", DemoScenario::Normal}, {"sensor-failure", DemoScenario::SensorFailure},
        {"timeout", DemoScenario::CommunicationTimeout}, {"recovery", DemoScenario::Recovery}
    }};
    for (const auto& scenario : scenarios) {
        const auto options = cli::parse_arguments({"--scenario", scenario.first});
        EXPECT_EQ(options.scenario, scenario.second);
        EXPECT_FALSE(options.summary_only);
    }
    EXPECT_EQ(cli::parse_arguments({"--concurrent"}).mode, cli::Mode::Concurrent);
    EXPECT_EQ(cli::parse_arguments({"-h"}).mode, cli::Mode::Help);
}

TEST(CliTest, SummaryOptionAcceptsEitherOrder)
{
    const auto first = cli::parse_arguments({"--summary-only", "--scenario", "recovery"});
    const auto last = cli::parse_arguments({"--scenario", "recovery", "--summary-only"});
    EXPECT_EQ(first.scenario, last.scenario);
    EXPECT_TRUE(first.summary_only);
    EXPECT_TRUE(last.summary_only);
}

TEST(CliTest, InvalidDuplicateAndConflictingOptionsProduceUsageErrorsWithoutSimulationOutput)
{
    const std::vector<std::vector<std::string_view>> cases{
        {"--scenario"}, {"--scenario", ""}, {"--scenario", "unknown"}, {"--unknown"},
        {"--scenario", "normal", "extra"}, {"--scenario", "normal", "--scenario", "recovery"},
        {"--summary-only", "--summary-only"}, {"--help", "--unknown"},
        {"--concurrent", "--scenario", "normal"}, {"--concurrent", "--summary-only"}
    };
    for (const auto& arguments : cases) {
        std::ostringstream output, error;
        EXPECT_EQ(cli::run(arguments, output, error), 2);
        EXPECT_TRUE(output.str().empty());
        EXPECT_NE(error.str().find("Error: "), std::string::npos);
        EXPECT_NE(error.str().find("Usage: vaecu_sim"), std::string::npos);
    }
}

TEST(CliTest, HelpDoesNotRunASimulation)
{
    std::ostringstream output, error;
    EXPECT_EQ(cli::run({"--help"}, output, error), 0);
    EXPECT_NE(output.str().find("sensor-failure|timeout|recovery"), std::string::npos);
    EXPECT_EQ(output.str().find("RUN started"), std::string::npos);
    EXPECT_TRUE(error.str().empty());
}

TEST(CliTest, SummaryHasExactStableAccountingAndNoDetailedLogs)
{
    std::ostringstream output, error;
    EXPECT_EQ(cli::run({"--scenario", "recovery", "--summary-only"}, output, error), 0);
    EXPECT_EQ(output.str(),
        "Virtual Automotive ECU Platform | scenario=recovery | deterministic\n"
        "Software CAN simulation | time=relative ms | routing=FIFO\n"
        "Summary: duration=1500ms CAN=12 routed=24 processed=24 dropped=0\n"
        "Diagnostics: raised=3 recovered=3 active=0 lost=0\n"
        "Shutdown: all ECUs OFFLINE; pending_discarded=0\n");
    EXPECT_TRUE(error.str().empty());
}

TEST(CliTest, DetailedLogsAreRepeatableAndIncludeWireBytesFaultsAndOnlineOfflineState)
{
    for (const auto scenario : {"normal", "sensor-failure", "timeout", "recovery"}) {
        std::ostringstream first, second, error;
        EXPECT_EQ(cli::run({"--scenario", scenario}, first, error), 0);
        EXPECT_EQ(cli::run({"--scenario", scenario}, second, error), 0);
        EXPECT_EQ(first.str(), second.str());
        EXPECT_TRUE(error.str().empty());
        EXPECT_NE(first.str().find("[0000 ms] CAN tx=Engine id=0x100 len=4 data=[DC 05 3C 00] routed=2 dropped=0"), std::string::npos);
        EXPECT_NE(first.str().find("[0250 ms] CAN tx=Scenario id=0x300 len=1 data=[01]"), std::string::npos);
        EXPECT_NE(first.str().find("ECU Engine=ONLINE Brake=ONLINE Dashboard=ONLINE"), std::string::npos);
        EXPECT_NE(first.str().find("ECU Engine=OFFLINE Brake=OFFLINE Dashboard=OFFLINE"), std::string::npos);
        if (std::string_view{scenario} == "recovery") {
            EXPECT_NE(first.str().find("[1000 ms] DIAG code=0x1104 ACTIVE engine communication timeout"), std::string::npos);
            EXPECT_NE(first.str().find("[1250 ms] DIAG code=0x1104 RECOVERED engine communication timeout"), std::string::npos);
        }
    }
}

TEST(CliTest, FormattingIgnoresAndPreservesCallerStreamFlags)
{
    std::ostringstream output;
    output << std::hex << std::setfill('*');
    const auto flags = output.flags();
    cli::print_report(run_demo_scenario(DemoScenario::Normal), true, output);
    EXPECT_NE(output.str().find("duration=500ms CAN=8 routed=16"), std::string::npos);
    EXPECT_EQ(output.flags(), flags);
    EXPECT_EQ(output.fill(), '*');
}

TEST(CliTest, OutputFailureReturnsRuntimeFailureInsteadOfSuccess)
{
    std::ostringstream output, error;
    output.setstate(std::ios::badbit);
    EXPECT_EQ(cli::run({}, output, error), 1);
    EXPECT_NE(error.str().find("Unable to write demonstration output"), std::string::npos);
    EXPECT_EQ(cli::run({"--help"}, output, error), 1);
    EXPECT_NO_THROW((void)run_demo_scenario(DemoScenario::Normal));
}

TEST(CliTest, ReportIgnoresAndPreservesCallerFieldWidth)
{
    const auto report = run_demo_scenario(DemoScenario::Normal);
    std::ostringstream expected, output;
    cli::print_report(report, true, expected);
    output << std::setfill('*');
    const auto width = static_cast<std::streamsize>(expected.str().size() + 10);
    output.width(width);
    cli::print_report(report, true, output);
    EXPECT_EQ(output.str(), expected.str());
    EXPECT_EQ(output.width(), width);
    EXPECT_EQ(output.fill(), '*');
}

class FailingSyncBuffer final : public std::stringbuf {
public:
    int sync() override { return -1; }
};

TEST(CliTest, BufferedOutputFailuresAreReportedInEveryMode)
{
    const std::array<std::vector<std::string_view>, 3> modes{{{}, {"--help"}, {"--concurrent"}}};
    for (const auto& arguments : modes) {
        SCOPED_TRACE(arguments.empty() ? "default" : arguments.front());
        for (const bool throwing : {false, true}) {
            SCOPED_TRACE(throwing);
            FailingSyncBuffer buffer;
            std::ostream output{&buffer};
            if (throwing) output.exceptions(std::ios::badbit | std::ios::failbit);
            std::ostringstream error;
            EXPECT_EQ(cli::run(arguments, output, error), 1);
            EXPECT_FALSE(error.str().empty());
        }
    }
}

} // namespace
