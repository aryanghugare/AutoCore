#include "vaecu/brake_ecu.hpp"

#include <stdexcept>

namespace vaecu {

BrakeEcu::BrakeEcu(ICanSender& sender, messages::BrakeStatus initial)
    : sender_{sender}, brake_{initial}
{
}

messages::BrakeStatus BrakeEcu::brake_state() const noexcept { return brake_; }
std::optional<SendResult> BrakeEcu::last_send_result() const noexcept { return last_send_; }
MessageDisposition BrakeEcu::last_message_result() const noexcept { return last_message_; }

void BrakeEcu::start(Timestamp now)
{
    if (running_ || stopped_) {
        throw std::logic_error("Brake ECU can only be started once");
    }
    last_now_ = now;
    running_ = true;
}

void BrakeEcu::advance(Timestamp now)
{
    if (!running_) {
        throw std::logic_error("Brake ECU is not running");
    }
    if (now < last_now_) {
        throw std::invalid_argument("Brake processing time must be monotonic");
    }
    last_now_ = now;
}

void BrakeEcu::on_tick(Timestamp now)
{
    advance(now);
    last_send_ = sender_.send(messages::encode_brake(brake_, now));
}

void BrakeEcu::on_message(const CanFrame& frame, Timestamp now)
{
    advance(now);
    if (frame.id() != messages::brake_event_id) {
        last_message_ = MessageDisposition::UnsupportedId;
        return;
    }
    const auto event = messages::decode_brake_event(frame);
    if (!event) {
        last_message_ = MessageDisposition::InvalidPayload;
        return;
    }
    if (frame.timestamp() > now) {
        last_message_ = MessageDisposition::InvalidTimestamp;
        return;
    }
    if (last_event_ && frame.timestamp() < *last_event_) {
        last_message_ = MessageDisposition::OutOfOrder;
        return;
    }
    brake_.applied = *event == messages::BrakeEvent::Apply;
    last_event_ = frame.timestamp();
    last_message_ = MessageDisposition::Accepted;
}

void BrakeEcu::stop() noexcept
{
    running_ = false;
    stopped_ = true;
}

} // namespace vaecu
