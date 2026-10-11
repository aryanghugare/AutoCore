#pragma once

#include "vaecu/brake_ecu.hpp"
#include "vaecu/dashboard_ecu.hpp"
#include "vaecu/diagnostics.hpp"
#include "vaecu/engine_ecu.hpp"
#include "vaecu/virtual_can_bus.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>

namespace vaecu {

struct ConcurrentConfig {
    std::chrono::milliseconds tick_period{10}; // Valid range: 1..60000 ms.
    std::size_t inbox_capacity{64};
    std::size_t event_capacity{64};
    messages::EngineTelemetry engine_inputs{1500, 60};
    bool publish_engine{true};
    bool publish_brake{true};
    DashboardConfig dashboard{};
    DiagnosticConfig diagnostics{};
};

struct TelemetrySnapshot {
    DashboardState state;
    std::uint64_t revision{0};
    Timestamp observed_at{};
};

// Owns four worker threads and every object they use. Each behavior object is
// confined to one worker; only copied snapshots and synchronized store values
// escape. Control methods are safe concurrently; destruction requires callers
// to release this object's lifetime first. Instances start once, never restart.
class ConcurrentSimulation final {
public:
    explicit ConcurrentSimulation(ConcurrentConfig config = {});
    ~ConcurrentSimulation();
    ConcurrentSimulation(const ConcurrentSimulation&) = delete;
    ConcurrentSimulation& operator=(const ConcurrentSimulation&) = delete;
    ConcurrentSimulation(ConcurrentSimulation&&) = delete;
    ConcurrentSimulation& operator=(ConcurrentSimulation&&) = delete;

    // Waits for all behaviors to start, then releases them together. A failed
    // start cancels and joins any already-created threads before throwing.
    void start();
    // Cancel and discard inboxes without joining. Before startup release this
    // also closes diagnostic reporting; otherwise its writer closes on exit.
    void request_stop() noexcept;
    void stop() noexcept;         // Idempotent cancellation and join.
    void rethrow_if_failed() const; // Worker exceptions never escape a thread.

    [[nodiscard]] SendResult send(const CanFrame& frame);
    void set_engine_publishing(bool enabled); // Takes effect by the next tick.
    void set_brake_publishing(bool enabled);
    [[nodiscard]] TelemetrySnapshot dashboard_snapshot() const;
    // Latest-value mailbox, not a history. nullopt on timeout/cancellation if
    // no newer revision exists. A final stopped snapshot is available after join.
    [[nodiscard]] std::optional<TelemetrySnapshot> wait_for_dashboard(
        std::uint64_t after_revision, Timestamp deadline);
    [[nodiscard]] std::optional<DiagnosticRecord> fault(FaultId id) const;
    [[nodiscard]] std::optional<DiagnosticEvent> take_event();
    [[nodiscard]] std::optional<DiagnosticEvent> wait_event_until(Timestamp deadline);
    [[nodiscard]] std::size_t discarded_events() const;

private:
    enum class State { Constructed, Running, Stopped };
    void worker(std::size_t index, Timestamp started_at) noexcept;
    void publish_dashboard(Timestamp now);
    void capture_failure() noexcept;
    void join_workers() noexcept;
    void require_running() const; // lifecycle_mutex_ held by caller.

    // Declaration order preserves dependencies through destruction.
    const ConcurrentConfig config_;
    VirtualCanBus bus_;
    DiagnosticStore store_;
    EngineEcu engine_;
    BrakeEcu brake_;
    DashboardEcu dashboard_;
    DiagnosticMonitor diagnostics_;
    const std::array<std::reference_wrapper<IEcu>, 4> tasks_;
    std::array<std::shared_ptr<CanReceiver>, 4> inboxes_;
    std::array<std::thread, 4> workers_;

    mutable std::mutex lifecycle_mutex_;
    State state_{State::Constructed};
    std::atomic<bool> stopping_{false};
    std::atomic<bool> engine_publishing_{true};
    std::atomic<bool> brake_publishing_{true};
    std::mutex startup_mutex_;
    std::condition_variable startup_ready_;
    std::size_t ready_workers_{0};
    bool released_{false};
    mutable std::mutex failure_mutex_;
    std::exception_ptr failure_;
    mutable std::mutex snapshot_mutex_;
    std::condition_variable snapshot_ready_;
    TelemetrySnapshot snapshot_;
};

} // namespace vaecu
