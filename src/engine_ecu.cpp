#include "vaecu/engine_ecu.hpp"

#include <stdexcept>

namespace vaecu {

EngineEcu::EngineEcu(ICanSender& sender, messages::EngineTelemetry inputs)
    : sender_{sender}, inputs_{inputs}
{
    if (!messages::is_valid(inputs)) {
        throw std::invalid_argument("Invalid simulated engine inputs");
    }
}

void EngineEcu::set_inputs(messages::EngineTelemetry inputs)
{
    if (stopped_) {
        throw std::logic_error("Cannot update a stopped engine ECU");
    }
    if (!messages::is_valid(inputs)) {
        throw std::invalid_argument("Invalid simulated engine inputs");
    }
    inputs_ = inputs;
}

messages::EngineTelemetry EngineEcu::inputs() const noexcept { return inputs_; }
std::optional<SendResult> EngineEcu::last_send_result() const noexcept { return last_send_; }
MessageDisposition EngineEcu::last_message_result() const noexcept { return last_message_; }

void EngineEcu::start(Timestamp now)
{
    if (running_ || stopped_) {
        throw std::logic_error("Engine ECU can only be started once");
    }
    last_now_ = now;
    running_ = true;
}

void EngineEcu::advance(Timestamp now)
{
    if (!running_) {
        throw std::logic_error("Engine ECU is not running");
    }
    if (now < last_now_) {
        throw std::invalid_argument("Engine processing time must be monotonic");
    }
    last_now_ = now;
}

void EngineEcu::on_tick(Timestamp now)
{
    advance(now);
    last_send_ = sender_.send(messages::encode_engine(inputs_, now));
}

void EngineEcu::on_message(const CanFrame&, Timestamp now)
{
    advance(now);
    // Engine input is supplied by the scenario, not by bus messages.
    last_message_ = MessageDisposition::UnsupportedId;
}

void EngineEcu::stop() noexcept
{
    running_ = false;
    stopped_ = true;
}

} // namespace vaecu
