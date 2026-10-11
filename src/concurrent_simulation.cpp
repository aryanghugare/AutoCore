#include "vaecu/concurrent_simulation.hpp"

#include <limits>
#include <stdexcept>

namespace vaecu {

ConcurrentSimulation::ConcurrentSimulation(ConcurrentConfig config)
    : config_{config}, store_{config.event_capacity}, engine_{bus_, config.engine_inputs},
      brake_{bus_}, dashboard_{config.dashboard}, diagnostics_{store_, config.diagnostics},
      tasks_{engine_, brake_, dashboard_, diagnostics_},
      engine_publishing_{config.publish_engine}, brake_publishing_{config.publish_brake}
{
    if (config.tick_period.count() < 1 || config.tick_period.count() > 60000) {
        throw std::invalid_argument("Concurrent tick period must be 1..60000 ms");
    }
    inboxes_[0] = bus_.register_receiver("engine", {}, config.inbox_capacity);
    inboxes_[1] = bus_.register_receiver("brake", {messages::brake_event_id}, config.inbox_capacity);
    inboxes_[2] = bus_.register_receiver("dashboard",
        {messages::engine_telemetry_id, messages::brake_status_id}, config.inbox_capacity);
    inboxes_[3] = bus_.register_receiver("diagnostics",
        {messages::engine_telemetry_id, messages::brake_status_id, messages::brake_event_id},
        config.inbox_capacity);
}

ConcurrentSimulation::~ConcurrentSimulation() { stop(); }

void ConcurrentSimulation::start()
{
    const std::lock_guard<std::mutex> lifecycle{lifecycle_mutex_};
    if (state_ != State::Constructed || stopping_) {
        throw std::logic_error("Concurrent simulation can only start once");
    }
    try {
        const auto started_at = Timestamp::clock::now();
        for (std::size_t index = 0; index < workers_.size(); ++index) {
            workers_[index] = std::thread{&ConcurrentSimulation::worker, this, index, started_at};
        }
        std::unique_lock<std::mutex> startup{startup_mutex_};
        startup_ready_.wait(startup, [this] { return ready_workers_ == workers_.size() || stopping_; });
        if (stopping_) {
            startup.unlock();
            rethrow_if_failed();
            throw std::runtime_error("Concurrent startup cancelled");
        }
        state_ = State::Running;
        released_ = true;
        startup.unlock();
        startup_ready_.notify_all();
    } catch (...) {
        request_stop();
        join_workers();
        store_.close_events();
        state_ = State::Stopped;
        throw;
    }
}

void ConcurrentSimulation::request_stop() noexcept
{
    bool close_events;
    {
        const std::lock_guard<std::mutex> lock{startup_mutex_};
        stopping_ = true;
        // Only released workers can update the diagnostic store. Cancellation
        // under the barrier mutex prevents start() from releasing them later.
        // Before release, no writer needs to finish and wake event readers.
        close_events = !released_;
    }
    (void)bus_.close();
    if (close_events) store_.close_events();
    startup_ready_.notify_all();
    // Lock the snapshot predicate's mutex after setting the flag to prevent a
    // notification from falling between predicate inspection and waiting.
    {
        const std::lock_guard<std::mutex> lock{snapshot_mutex_};
    }
    snapshot_ready_.notify_all();
}

void ConcurrentSimulation::join_workers() noexcept
{
    for (auto& worker_thread : workers_) {
        if (worker_thread.joinable()) worker_thread.join();
    }
}

void ConcurrentSimulation::stop() noexcept
{
    const std::lock_guard<std::mutex> lifecycle{lifecycle_mutex_};
    if (state_ == State::Stopped) return;
    request_stop();
    join_workers();
    store_.close_events();
    state_ = State::Stopped;
}

void ConcurrentSimulation::capture_failure() noexcept
{
    {
        const std::lock_guard<std::mutex> lock{failure_mutex_};
        if (!failure_) failure_ = std::current_exception();
    }
    request_stop();
}

void ConcurrentSimulation::rethrow_if_failed() const
{
    std::exception_ptr failure;
    {
        const std::lock_guard<std::mutex> lock{failure_mutex_};
        failure = failure_;
    }
    if (failure) std::rethrow_exception(failure);
}

void ConcurrentSimulation::worker(std::size_t index, Timestamp started_at) noexcept
{
    auto& ecu = tasks_[index].get();
    try {
        ecu.start(started_at);
        {
            std::unique_lock<std::mutex> startup{startup_mutex_};
            ++ready_workers_;
            startup_ready_.notify_all();
            startup_ready_.wait(startup, [this] { return released_ || stopping_; });
        }
        if (index == 3 && !stopping_) {
            diagnostics_.observe_engine_inputs(config_.engine_inputs.engine_rpm,
                config_.engine_inputs.vehicle_speed_kph, Timestamp::clock::now());
        }
        auto next_tick = Timestamp::clock::now();
        while (!stopping_) {
            const auto received = inboxes_[index]->receive_until(next_tick);
            if (stopping_) break;
            auto now = Timestamp::clock::now();
            if (const auto* frame = std::get_if<CanFrame>(&received)) {
                ecu.on_message(*frame, now);
            } else if (std::get<ReceiveStatus>(received) == ReceiveStatus::Closed) {
                break;
            }
            // Check after every frame so a busy inbox cannot starve ticks.
            now = Timestamp::clock::now();
            if (now >= next_tick && !stopping_) {
                const bool publish = index == 0 ? engine_publishing_.load() :
                    (index == 1 ? brake_publishing_.load() : true);
                if (publish) ecu.on_tick(now);
                // Skip missed periods instead of creating a catch-up burst.
                next_tick = now + config_.tick_period;
            }
            if (index == 2) publish_dashboard(now);
        }
    } catch (...) {
        capture_failure();
    }
    ecu.stop();
    if (index == 2) publish_dashboard(Timestamp::clock::now());
    if (index == 3) store_.close_events();
}

void ConcurrentSimulation::publish_dashboard(Timestamp now)
{
    {
        const std::lock_guard<std::mutex> lock{snapshot_mutex_};
        snapshot_.state = dashboard_.state();
        snapshot_.observed_at = now;
        if (snapshot_.revision != std::numeric_limits<std::uint64_t>::max()) ++snapshot_.revision;
    }
    snapshot_ready_.notify_all();
}

void ConcurrentSimulation::require_running() const
{
    if (state_ != State::Running || stopping_) {
        throw std::logic_error("Concurrent simulation is not running");
    }
}

SendResult ConcurrentSimulation::send(const CanFrame& frame)
{
    const std::lock_guard<std::mutex> lifecycle{lifecycle_mutex_};
    if (state_ != State::Running || stopping_) return {SendStatus::Closed, 0, 0};
    return bus_.send(frame);
}

void ConcurrentSimulation::set_engine_publishing(bool enabled)
{
    const std::lock_guard<std::mutex> lifecycle{lifecycle_mutex_};
    require_running();
    engine_publishing_ = enabled;
}

void ConcurrentSimulation::set_brake_publishing(bool enabled)
{
    const std::lock_guard<std::mutex> lifecycle{lifecycle_mutex_};
    require_running();
    brake_publishing_ = enabled;
}

TelemetrySnapshot ConcurrentSimulation::dashboard_snapshot() const
{
    const std::lock_guard<std::mutex> lock{snapshot_mutex_};
    return snapshot_;
}

std::optional<TelemetrySnapshot> ConcurrentSimulation::wait_for_dashboard(
    std::uint64_t after_revision, Timestamp deadline)
{
    std::unique_lock<std::mutex> lock{snapshot_mutex_};
    if (deadline > Timestamp::clock::now()) {
        snapshot_ready_.wait_until(lock, deadline, [this, after_revision] {
            return snapshot_.revision > after_revision || stopping_;
        });
    }
    if (snapshot_.revision > after_revision) return snapshot_;
    return std::nullopt;
}

std::optional<DiagnosticRecord> ConcurrentSimulation::fault(FaultId id) const
{
    return store_.record(id);
}
std::optional<DiagnosticEvent> ConcurrentSimulation::take_event() { return store_.take_event(); }
std::optional<DiagnosticEvent> ConcurrentSimulation::wait_event_until(Timestamp deadline)
{
    return store_.wait_event_until(deadline);
}
std::size_t ConcurrentSimulation::discarded_events() const { return store_.discarded_events(); }

} // namespace vaecu
