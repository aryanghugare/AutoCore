#include "vaecu/messages.hpp"

#include <stdexcept>

namespace vaecu::messages {
namespace {

void write_u16(CanFrame::Payload& payload, std::size_t offset, std::uint16_t value) noexcept
{
    payload[offset] = static_cast<std::uint8_t>(value & 0xFFU);
    payload[offset + 1] = static_cast<std::uint8_t>(value >> 8U);
}

std::uint16_t read_u16(const CanFrame::Payload& payload, std::size_t offset) noexcept
{
    return static_cast<std::uint16_t>(static_cast<unsigned>(payload[offset]) |
        (static_cast<unsigned>(payload[offset + 1]) << 8U));
}

} // namespace

bool is_valid(EngineTelemetry value) noexcept
{
    return value.engine_rpm <= max_engine_rpm &&
        value.vehicle_speed_kph <= max_vehicle_speed_kph;
}

CanFrame encode_engine(EngineTelemetry value, Timestamp timestamp)
{
    if (!is_valid(value)) {
        throw std::invalid_argument("Engine telemetry is outside simulation limits");
    }
    CanFrame::Payload payload{};
    write_u16(payload, 0, value.engine_rpm);
    write_u16(payload, 2, value.vehicle_speed_kph);
    return {engine_telemetry_id, payload, 4, timestamp};
}

CanFrame encode_brake(BrakeStatus value, Timestamp timestamp)
{
    return {brake_status_id, {static_cast<std::uint8_t>(value.applied)}, 1, timestamp};
}

CanFrame encode_brake_event(BrakeEvent event, Timestamp timestamp)
{
    if (event != BrakeEvent::Apply && event != BrakeEvent::Release) {
        throw std::invalid_argument("Unsupported simulated brake event");
    }
    return {brake_event_id, {static_cast<std::uint8_t>(event)}, 1, timestamp};
}

std::optional<EngineTelemetry> decode_engine(const CanFrame& frame) noexcept
{
    if (frame.id() != engine_telemetry_id || frame.payload_size() != 4) {
        return std::nullopt;
    }
    const EngineTelemetry value{read_u16(frame.payload(), 0), read_u16(frame.payload(), 2)};
    return is_valid(value) ? std::optional<EngineTelemetry>{value} : std::nullopt;
}

std::optional<BrakeStatus> decode_brake(const CanFrame& frame) noexcept
{
    if (frame.id() != brake_status_id || frame.payload_size() != 1 || frame.payload()[0] > 1) {
        return std::nullopt;
    }
    return BrakeStatus{frame.payload()[0] == 1};
}

std::optional<BrakeEvent> decode_brake_event(const CanFrame& frame) noexcept
{
    if (frame.id() != brake_event_id || frame.payload_size() != 1) {
        return std::nullopt;
    }
    if (frame.payload()[0] == static_cast<std::uint8_t>(BrakeEvent::Apply)) {
        return BrakeEvent::Apply;
    }
    if (frame.payload()[0] == static_cast<std::uint8_t>(BrakeEvent::Release)) {
        return BrakeEvent::Release;
    }
    return std::nullopt;
}

} // namespace vaecu::messages
