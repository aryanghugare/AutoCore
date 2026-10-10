#include "vaecu/can_frame.hpp"

#include <stdexcept>

namespace vaecu {

CanFrame::CanFrame(CanId id, Payload payload, std::size_t payload_size,
                   Timestamp timestamp)
    : id_{id}, payload_{payload}, payload_size_{payload_size}, timestamp_{timestamp}
{
    if (!is_valid_payload_size(payload_size)) {
        throw std::length_error("CAN-inspired payload must contain at most eight bytes");
    }

    for (std::size_t index = payload_size_; index < payload_.size(); ++index) {
        payload_[index] = 0;
    }
}

CanId CanFrame::id() const noexcept
{
    return id_;
}

const CanFrame::Payload& CanFrame::payload() const noexcept
{
    return payload_;
}

std::size_t CanFrame::payload_size() const noexcept
{
    return payload_size_;
}

Timestamp CanFrame::timestamp() const noexcept
{
    return timestamp_;
}

} // namespace vaecu
