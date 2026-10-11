#include "bridge_json.hpp"

#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

// The parent owns stdin. Sending "stop\n" or closing it cancels gracefully.
// Natural completion emits a summary, then the parent closes stdin so the
// reader can join. No detached threads and no platform-specific signals.
int main(int argc, char* argv[])
{
    if (argc != 3 || std::string{argv[1]} != "--scenario") {
        std::cerr << "Usage: vaecu_bridge --scenario normal|sensor-failure|timeout|recovery\n";
        return 2;
    }
    vaecu::DemoScenario scenario;
    try { scenario = vaecu::bridge::parse_scenario(argv[2]); }
    catch (const std::invalid_argument& error) { std::cerr << error.what() << '\n'; return 2; }
    std::mutex mutex;
    std::condition_variable wake;
    bool cancelled = false;
    bool invalid_command = false;
    const auto epoch = vaecu::Timestamp::clock::now();
    std::thread reader;
    int result = 0;
    try {
        reader = std::thread{[&] {
            char command[16]{};
            const bool received = static_cast<bool>(std::cin.getline(command, 16));
            {
                const std::lock_guard<std::mutex> lock{mutex};
                invalid_command = (received && std::string{command} != "stop") ||
                    (!received && !std::cin.eof());
                cancelled = true;
            }
            wake.notify_all();
        }};
        auto report = vaecu::run_demo_scenario(scenario, {
            [&](std::chrono::milliseconds elapsed) {
                std::unique_lock<std::mutex> lock{mutex};
                wake.wait_until(lock, epoch + elapsed * 4, [&] { return cancelled; });
                return !cancelled;
            },
            [](const vaecu::DemoEvent& event) {
                std::cout << vaecu::bridge::event_json(event) << '\n' << std::flush;
                if (!std::cout) throw std::runtime_error("Bridge output failed");
            }
        });
        bool stopped;
        {
            const std::lock_guard<std::mutex> lock{mutex};
            stopped = cancelled;
            if (invalid_command) throw std::invalid_argument("Unsupported bridge command; use stop");
        }
        std::cout << vaecu::bridge::summary_json(report.summary, stopped) << '\n' << std::flush;
        if (!std::cout) throw std::runtime_error("Bridge summary output failed");
    } catch (const std::exception& error) {
        std::cerr << "Bridge failed: " << error.what() << '\n';
        result = 1;
    }
    // Parent closes stdin upon summary or error; bounded parent shutdown also
    // handles a broken pipe/unresponsive process. Reader owns all stdin access.
    if (reader.joinable()) reader.join();
    return result;
}
