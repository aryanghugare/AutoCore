#pragma once

#include "vaecu/ecu.hpp"
#include "vaecu/messages.hpp"

#include <chrono>
#include <optional>

namespace vaecu {

enum class SignalHealth { Missing, Fresh, Stale };

struct DashboardConfig {
    Timestamp::duration engine_timeout{std::chrono::milliseconds{500}};
    Timestamp::duration brake_timeout{std::chrono::milliseconds{500}};
};

// Owned snapshot: absent means never received, stale values remain available
// only alongside their explicit health flag. No UI or transport ownership.
struct DashboardState {
    std::optional<messages::EngineTelemetry> engine;
    std::optional<messages::BrakeStatus> brake;
    SignalHealth engine_health{SignalHealth::Missing};
    SignalHealth brake_health{SignalHealth::Missing};
};

class DashboardEcu final : public IEcu {
public:
    explicit DashboardEcu(DashboardConfig config = {});
    [[nodiscard]] DashboardState state() const noexcept;
    [[nodiscard]] MessageDisposition last_message_result() const noexcept;

    void start(Timestamp now) override;
    void on_tick(Timestamp now) override;
    void on_message(const CanFrame& frame, Timestamp now) override;
    void stop() noexcept override;

private:
    void advance(Timestamp now);
    void refresh_health(Timestamp now) noexcept;
    DashboardConfig config_;
    DashboardState state_;
    std::optional<Timestamp> engine_time_;
    std::optional<Timestamp> brake_time_;
    Timestamp last_now_{};
    MessageDisposition last_message_{MessageDisposition::None};
    bool running_{false};
    bool stopped_{false};
};

} // namespace vaecu
