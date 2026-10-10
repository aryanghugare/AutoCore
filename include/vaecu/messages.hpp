#pragma once

#include "vaecu/can_frame.hpp"

#include <cstdint>
#include <optional>

namespace vaecu::messages {

// Canonical simulation schema; see docs/messages.md for units and byte layout.
inline const CanId engine_telemetry_id{0x100};
inline const CanId brake_status_id{0x200};
inline const CanId brake_event_id{0x300};
inline constexpr std::uint16_t max_engine_rpm = 9000;
inline constexpr std::uint16_t max_vehicle_speed_kph = 300;

struct EngineTelemetry {
    std::uint16_t engine_rpm{0};
    std::uint16_t vehicle_speed_kph{0};
};

struct BrakeStatus {
    bool applied{false};
};

enum class BrakeEvent : std::uint8_t { Apply = 1, Release = 2 };

[[nodiscard]] bool is_valid(EngineTelemetry value) noexcept;
// Encoders reject invalid application values with std::invalid_argument.
[[nodiscard]] CanFrame encode_engine(EngineTelemetry value, Timestamp timestamp);
[[nodiscard]] CanFrame encode_brake(BrakeStatus value, Timestamp timestamp);
[[nodiscard]] CanFrame encode_brake_event(BrakeEvent event, Timestamp timestamp);
// Decoders require an exact ID, exact length, and valid application values.
[[nodiscard]] std::optional<EngineTelemetry> decode_engine(const CanFrame& frame) noexcept;
[[nodiscard]] std::optional<BrakeStatus> decode_brake(const CanFrame& frame) noexcept;
[[nodiscard]] std::optional<BrakeEvent> decode_brake_event(const CanFrame& frame) noexcept;

} // namespace vaecu::messages
