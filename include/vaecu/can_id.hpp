#pragma once

#include <cstdint>

namespace vaecu {

// An 11-bit CAN-inspired identifier. This does not model extended CAN IDs.
class CanId final {
public:
    static constexpr std::uint16_t max_value = 0x7FF;

    // Throws std::out_of_range for a value outside [0, max_value].
    explicit CanId(std::int64_t value);

    [[nodiscard]] static constexpr bool is_valid(std::int64_t value) noexcept
    {
        return value >= 0 && value <= max_value;
    }

    [[nodiscard]] constexpr std::uint16_t value() const noexcept
    {
        return value_;
    }

    friend bool operator==(CanId lhs, CanId rhs) noexcept
    {
        return lhs.value_ == rhs.value_;
    }

    friend bool operator!=(CanId lhs, CanId rhs) noexcept
    {
        return !(lhs == rhs);
    }

    friend bool operator<(CanId lhs, CanId rhs) noexcept
    {
        return lhs.value_ < rhs.value_;
    }

private:
    std::uint16_t value_;
};

} // namespace vaecu
