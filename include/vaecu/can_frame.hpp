#pragma once

#include "vaecu/can_id.hpp"
#include "vaecu/time.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace vaecu {

// A validated value with fixed storage. No CAN electrical/protocol behavior.
class CanFrame final {
public:
    static constexpr std::size_t max_payload_size = 8;
    using Payload = std::array<std::uint8_t, max_payload_size>;

    // Throws std::length_error if payload_size exceeds eight. Bytes outside
    // the active payload are normalized to zero. Timestamp is supplied by
    // the caller, so tests do not need wall-clock access.
    CanFrame(CanId id, Payload payload, std::size_t payload_size,
             Timestamp timestamp);

    [[nodiscard]] static constexpr bool is_valid_payload_size(
        std::size_t payload_size) noexcept
    {
        return payload_size <= max_payload_size;
    }

    [[nodiscard]] CanId id() const noexcept;
    [[nodiscard]] const Payload& payload() const noexcept;
    [[nodiscard]] std::size_t payload_size() const noexcept;
    [[nodiscard]] Timestamp timestamp() const noexcept;

private:
    CanId id_;
    Payload payload_;
    std::size_t payload_size_;
    Timestamp timestamp_;
};

} // namespace vaecu
