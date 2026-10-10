#include "vaecu/can_id.hpp"

#include <stdexcept>

namespace vaecu {

CanId::CanId(std::int64_t value)
    : value_{0}
{
    if (!is_valid(value)) {
        throw std::out_of_range("CAN-inspired identifier must be in [0, 0x7FF]");
    }

    value_ = static_cast<std::uint16_t>(value);
}

} // namespace vaecu
