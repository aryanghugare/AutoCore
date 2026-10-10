#include "vaecu/dashboard_ecu.hpp"

#include <stdexcept>

namespace vaecu {
namespace {

SignalHealth health(std::optional<Timestamp> updated, Timestamp now,
                    Timestamp::duration timeout) noexcept
{
    if (!updated) {
        return SignalHealth::Missing;
    }
    // Avoid overflow at extreme synthetic timestamps. A deadline beyond the
    // representable maximum cannot be reached by a representable 'now'.
    if (*updated > Timestamp::max() - timeout) {
        return SignalHealth::Fresh;
    }
    return now >= *updated + timeout ? SignalHealth::Stale : SignalHealth::Fresh;
}

} // namespace

DashboardEcu::DashboardEcu(DashboardConfig config) : config_{config}
{
    if (config.engine_timeout <= Timestamp::duration::zero() ||
        config.brake_timeout <= Timestamp::duration::zero()) {
        throw std::invalid_argument("Dashboard timeouts must be positive");
    }
}

DashboardState DashboardEcu::state() const noexcept { return state_; }
MessageDisposition DashboardEcu::last_message_result() const noexcept { return last_message_; }

void DashboardEcu::start(Timestamp now)
{
    if (running_ || stopped_) {
        throw std::logic_error("Dashboard ECU can only be started once");
    }
    last_now_ = now;
    running_ = true;
}

void DashboardEcu::advance(Timestamp now)
{
    if (!running_) {
        throw std::logic_error("Dashboard ECU is not running");
    }
    if (now < last_now_) {
        throw std::invalid_argument("Dashboard processing time must be monotonic");
    }
    last_now_ = now;
    refresh_health(now);
}

void DashboardEcu::refresh_health(Timestamp now) noexcept
{
    state_.engine_health = health(engine_time_, now, config_.engine_timeout);
    state_.brake_health = health(brake_time_, now, config_.brake_timeout);
}

void DashboardEcu::on_tick(Timestamp now) { advance(now); }

void DashboardEcu::on_message(const CanFrame& frame, Timestamp now)
{
    advance(now);
    const bool engine = frame.id() == messages::engine_telemetry_id;
    const bool brake = frame.id() == messages::brake_status_id;
    if (!engine && !brake) {
        last_message_ = MessageDisposition::UnsupportedId;
        return;
    }
    const auto engine_value = engine ? messages::decode_engine(frame) : std::nullopt;
    const auto brake_value = brake ? messages::decode_brake(frame) : std::nullopt;
    if ((engine && !engine_value) || (brake && !brake_value)) {
        last_message_ = MessageDisposition::InvalidPayload;
        return;
    }
    if (frame.timestamp() > now) {
        last_message_ = MessageDisposition::InvalidTimestamp;
        return;
    }
    auto& last_time = engine ? engine_time_ : brake_time_;
    if (last_time && frame.timestamp() < *last_time) {
        last_message_ = MessageDisposition::OutOfOrder;
        return;
    }
    if (engine) {
        state_.engine = *engine_value;
    } else {
        state_.brake = *brake_value;
    }
    last_time = frame.timestamp();
    last_message_ = MessageDisposition::Accepted;
    refresh_health(now);
}

void DashboardEcu::stop() noexcept
{
    running_ = false;
    stopped_ = true;
    // Retain the last data, but never expose a stopped display as fresh.
    if (state_.engine) state_.engine_health = SignalHealth::Stale;
    if (state_.brake) state_.brake_health = SignalHealth::Stale;
}

} // namespace vaecu
