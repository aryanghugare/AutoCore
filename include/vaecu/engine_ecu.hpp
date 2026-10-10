#pragma once

#include "vaecu/can_transport.hpp"
#include "vaecu/ecu.hpp"
#include "vaecu/messages.hpp"

#include <optional>

namespace vaecu {

class EngineEcu final : public IEcu {
public:
    // Sender is non-owning and must outlive this ECU. Inputs use schema units.
    explicit EngineEcu(ICanSender& sender, messages::EngineTelemetry inputs = {});
    void set_inputs(messages::EngineTelemetry inputs);
    [[nodiscard]] messages::EngineTelemetry inputs() const noexcept;
    [[nodiscard]] std::optional<SendResult> last_send_result() const noexcept;
    [[nodiscard]] MessageDisposition last_message_result() const noexcept;

    void start(Timestamp now) override;
    void on_tick(Timestamp now) override;
    void on_message(const CanFrame& frame, Timestamp now) override;
    void stop() noexcept override;

private:
    void advance(Timestamp now);
    ICanSender& sender_;
    messages::EngineTelemetry inputs_;
    std::optional<SendResult> last_send_;
    Timestamp last_now_{};
    MessageDisposition last_message_{MessageDisposition::None};
    bool running_{false};
    bool stopped_{false};
};

} // namespace vaecu
