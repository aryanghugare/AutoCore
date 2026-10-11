#include "vaecu/diagnostics.hpp"

#include "vaecu/messages.hpp"

#include <limits>
#include <stdexcept>

namespace vaecu {
namespace {

std::size_t fault_index(FaultId id)
{
    for (std::size_t index = 0; index < fault_definitions.size(); ++index) {
        if (fault_definitions[index].id == id) return index;
    }
    throw std::invalid_argument("Unknown project diagnostic fault identifier");
}

void increment(std::size_t& count) noexcept
{
    if (count != std::numeric_limits<std::size_t>::max()) ++count;
}

bool expired(Timestamp origin, Timestamp now, Timestamp::duration duration) noexcept
{
    // Duration is positive. Guard the addition rather than subtracting time
    // points, which could overflow for extreme injected timestamps.
    return origin <= Timestamp::max() - duration && now >= origin + duration;
}

inline constexpr std::array<FaultId, 3> payload_faults{
    FaultId::EnginePayloadInvalid, FaultId::BrakePayloadInvalid, FaultId::BrakeEventPayloadInvalid};

struct StreamFaults {
    FaultId missing;
    FaultId stale;
    FaultId timeout;
};

inline constexpr std::array<StreamFaults, 2> periodic_faults{{
    {FaultId::EngineMessageMissing, FaultId::EngineMessageStale, FaultId::EngineCommunicationTimeout},
    {FaultId::BrakeMessageMissing, FaultId::BrakeMessageStale, FaultId::BrakeCommunicationTimeout}
}};

} // namespace

DiagnosticStore::DiagnosticStore(std::size_t event_capacity) : events_(event_capacity)
{
    if (event_capacity == 0) {
        throw std::invalid_argument("Diagnostic event capacity must be positive");
    }
}

void DiagnosticStore::set_fault(FaultId id, bool active, Timestamp now)
{
    const std::lock_guard<std::mutex> lock{mutex_};
    if (events_closed_) throw std::logic_error("Diagnostic event feed is closed");
    const auto index = fault_index(id);
    if (last_now_ && now < *last_now_) {
        throw std::invalid_argument("Diagnostic store time must be monotonic");
    }
    last_now_ = now;
    auto& entry = records_[index];
    if (!entry) {
        if (!active) return;
        entry = DiagnosticRecord{id, FaultStatus::Active, now, now, std::nullopt, 1};
    } else {
        if ((entry->status == FaultStatus::Active) == active) return;
        entry->status = active ? FaultStatus::Active : FaultStatus::Cleared;
        if (active) {
            entry->last_occurrence = now;
            increment(entry->occurrence_count);
        } else {
            entry->last_recovery = now;
        }
    }
    append_event({id, active ? DiagnosticEventKind::Raised : DiagnosticEventKind::Recovered, now});
    event_ready_.notify_one();
}

std::optional<DiagnosticRecord> DiagnosticStore::record(FaultId id) const
{
    const std::lock_guard<std::mutex> lock{mutex_};
    return records_[fault_index(id)];
}

void DiagnosticStore::append_event(DiagnosticEvent event) noexcept
{
    if (size_ == events_.size()) {
        events_[head_] = event;
        head_ = (head_ + 1) % events_.size();
        increment(discarded_);
    } else {
        events_[(head_ + size_) % events_.size()] = event;
        ++size_;
    }
}

std::optional<DiagnosticEvent> DiagnosticStore::take_event() noexcept
{
    const std::lock_guard<std::mutex> lock{mutex_};
    return pop_event_locked();
}

std::optional<DiagnosticEvent> DiagnosticStore::wait_event_until(Timestamp deadline)
{
    std::unique_lock<std::mutex> lock{mutex_};
    if (deadline > Timestamp::clock::now()) {
        event_ready_.wait_until(lock, deadline, [this] { return size_ != 0 || events_closed_; });
    }
    return pop_event_locked();
}

void DiagnosticStore::close_events() noexcept
{
    {
        const std::lock_guard<std::mutex> lock{mutex_};
        events_closed_ = true;
    }
    event_ready_.notify_all();
}

bool DiagnosticStore::events_closed() const noexcept
{
    const std::lock_guard<std::mutex> lock{mutex_};
    return events_closed_;
}

std::optional<DiagnosticEvent> DiagnosticStore::pop_event_locked() noexcept
{
    if (size_ == 0) return std::nullopt;
    auto event = events_[head_];
    events_[head_].reset();
    head_ = (head_ + 1) % events_.size();
    --size_;
    return event;
}

std::size_t DiagnosticStore::pending_events() const noexcept
{
    const std::lock_guard<std::mutex> lock{mutex_};
    return size_;
}
std::size_t DiagnosticStore::discarded_events() const noexcept
{
    const std::lock_guard<std::mutex> lock{mutex_};
    return discarded_;
}

DiagnosticMonitor::DiagnosticMonitor(IDiagnosticSink& sink, DiagnosticConfig config)
    : sink_{sink}, config_{config}
{
    if (config.startup_grace <= Timestamp::duration::zero() ||
        config.stale_after <= Timestamp::duration::zero() ||
        config.communication_timeout <= Timestamp::duration::zero()) {
        throw std::invalid_argument("Diagnostic durations must be positive");
    }
}

void DiagnosticMonitor::start(Timestamp now)
{
    if (running_ || stopped_) {
        throw std::logic_error("Diagnostic monitor can only be started once");
    }
    started_at_ = now;
    last_now_ = now;
    running_ = true;
}

void DiagnosticMonitor::advance(Timestamp now)
{
    if (!running_) throw std::logic_error("Diagnostic monitor is not running");
    if (now < last_now_) throw std::invalid_argument("Diagnostic processing time must be monotonic");
    last_now_ = now;
}

void DiagnosticMonitor::emit(FaultId id, bool active, Timestamp now)
{
    auto& previous = active_[fault_index(id)];
    if (previous == active) return;
    sink_.set_fault(id, active, now);
    // Commit only after the sink accepts the transition; a throwing sink can
    // be retried at the same or a later processing time.
    previous = active;
}

void DiagnosticMonitor::evaluate(Timestamp now)
{
    for (std::size_t index = 0; index < periodic_faults.size(); ++index) {
        const auto& stream = streams_[index];
        const auto faults = periodic_faults[index];
        emit(faults.missing, !stream.publication && expired(started_at_, now, config_.startup_grace), now);
        emit(faults.stale, stream.publication && expired(*stream.publication, now, config_.stale_after), now);
        emit(faults.timeout, expired(stream.received.value_or(started_at_), now,
                                    config_.communication_timeout), now);
    }
}

void DiagnosticMonitor::on_tick(Timestamp now)
{
    advance(now);
    evaluate(now);
}

void DiagnosticMonitor::observe_stream(std::size_t index, bool valid_payload,
                                      const CanFrame& frame, Timestamp now)
{
    auto& stream = streams_[index];
    stream.received = now; // Activity is distinct from usable application data.
    if (!valid_payload) {
        emit(payload_faults[index], true, now);
        return;
    }
    if (frame.timestamp() > now ||
        (stream.publication && frame.timestamp() < *stream.publication)) return;
    stream.publication = frame.timestamp();
    emit(payload_faults[index], false, now);
}

void DiagnosticMonitor::on_message(const CanFrame& frame, Timestamp now)
{
    advance(now);
    if (frame.id() == messages::engine_telemetry_id) {
        observe_stream(0, messages::decode_engine(frame).has_value(), frame, now);
    } else if (frame.id() == messages::brake_status_id) {
        observe_stream(1, messages::decode_brake(frame).has_value(), frame, now);
    } else if (frame.id() == messages::brake_event_id) {
        observe_stream(2, messages::decode_brake_event(frame).has_value(), frame, now);
    }
    evaluate(now);
}

void DiagnosticMonitor::observe_engine_inputs(std::int64_t engine_rpm,
                                             std::int64_t vehicle_speed_kph, Timestamp now)
{
    advance(now);
    emit(FaultId::EngineSpeedSensorInvalid,
         engine_rpm < 0 || engine_rpm > messages::max_engine_rpm, now);
    emit(FaultId::VehicleSpeedSensorInvalid,
         vehicle_speed_kph < 0 || vehicle_speed_kph > messages::max_vehicle_speed_kph, now);
    evaluate(now);
}

void DiagnosticMonitor::stop() noexcept
{
    running_ = false;
    stopped_ = true;
    // Stopping observation is not evidence that active faults have recovered.
}

} // namespace vaecu
