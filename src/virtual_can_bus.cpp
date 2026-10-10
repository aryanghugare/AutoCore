#include "vaecu/virtual_can_bus.hpp"

#include <stdexcept>
#include <type_traits>
#include <utility>

namespace vaecu {

static_assert(std::is_nothrow_copy_constructible_v<CanFrame>);
static_assert(std::is_nothrow_move_constructible_v<ReceiveResult>);

CanReceiver::CanReceiver(std::initializer_list<CanId> accepted_ids,
                         std::size_t capacity)
    : slots_(capacity)
{
    if (capacity == 0) {
        throw std::invalid_argument("Receiver queue capacity must be positive");
    }
    for (const CanId id : accepted_ids) {
        filter_.set(id.value());
    }
}

bool CanReceiver::accepts(CanId id) const noexcept
{
    return filter_[id.value()];
}

bool CanReceiver::enqueue(const CanFrame& frame) noexcept
{
    if (closed_ || size_ == slots_.size()) {
        return false;
    }
    const std::size_t tail = (head_ + size_) % slots_.size();
    slots_[tail].emplace(frame);
    ++size_;
    return true;
}

ReceiveResult CanReceiver::try_receive() noexcept
{
    if (closed_) {
        return ReceiveStatus::Closed;
    }
    if (size_ == 0) {
        return ReceiveStatus::Empty;
    }

    ReceiveResult result{*slots_[head_]};
    slots_[head_].reset();
    head_ = (head_ + 1) % slots_.size();
    --size_;
    return result;
}

std::size_t CanReceiver::capacity() const noexcept
{
    return slots_.size();
}

std::size_t CanReceiver::pending() const noexcept
{
    return size_;
}

bool CanReceiver::is_closed() const noexcept
{
    return closed_;
}

std::size_t CanReceiver::close() noexcept
{
    const std::size_t discarded = size_;
    closed_ = true;
    for (auto& slot : slots_) {
        slot.reset();
    }
    head_ = 0;
    size_ = 0;
    return discarded;
}

VirtualCanBus::VirtualCanBus(std::size_t max_receivers)
    : max_receivers_{max_receivers}
{
    if (max_receivers == 0) {
        throw std::invalid_argument("Bus receiver limit must be positive");
    }
    receivers_.reserve(max_receivers);
}

VirtualCanBus::~VirtualCanBus()
{
    (void)close();
}

std::shared_ptr<CanReceiver> VirtualCanBus::register_receiver(
    const std::string& name, std::initializer_list<CanId> accepted_ids,
    std::size_t capacity)
{
    if (closed_) {
        throw std::logic_error("Cannot register a receiver on a closed bus");
    }
    if (name.empty() || receiver(name)) {
        throw std::invalid_argument("Receiver name must be nonempty and unique");
    }
    if (receivers_.size() == max_receivers_) {
        throw std::length_error("Bus receiver registration limit reached");
    }

    auto inbox = std::make_shared<CanReceiver>(accepted_ids, capacity);
    receivers_.push_back(Registration{name, inbox});
    return inbox;
}

std::shared_ptr<CanReceiver> VirtualCanBus::receiver(const std::string& name) const
{
    for (const auto& entry : receivers_) {
        if (entry.name == name) {
            return entry.inbox;
        }
    }
    return {};
}

bool VirtualCanBus::stop_receiver(const std::string& name) noexcept
{
    for (const auto& entry : receivers_) {
        if (entry.name == name) {
            (void)entry.inbox->close();
            return true;
        }
    }
    return false;
}

SendResult VirtualCanBus::send(const CanFrame& frame) noexcept
{
    if (closed_) {
        return {SendStatus::Closed, 0, 0};
    }
    if (!CanId::is_valid(frame.id().value()) ||
        !CanFrame::is_valid_payload_size(frame.payload_size())) {
        return {SendStatus::InvalidFrame, 0, 0};
    }

    SendResult result{SendStatus::Routed, 0, 0};
    for (const auto& entry : receivers_) {
        if (entry.inbox->is_closed() || !entry.inbox->accepts(frame.id())) {
            continue;
        }
        if (entry.inbox->enqueue(frame)) {
            ++result.delivered;
        } else {
            ++result.dropped;
        }
    }
    return result;
}

SendResult VirtualCanBus::send_raw(std::int64_t identifier,
    const CanFrame::Payload& payload, std::size_t payload_size, Timestamp timestamp)
{
    if (closed_) {
        return {SendStatus::Closed, 0, 0};
    }
    if (!CanId::is_valid(identifier) ||
        !CanFrame::is_valid_payload_size(payload_size)) {
        return {SendStatus::InvalidFrame, 0, 0};
    }
    return send(CanFrame{CanId{identifier}, payload, payload_size, timestamp});
}

bool VirtualCanBus::is_closed() const noexcept
{
    return closed_;
}

std::size_t VirtualCanBus::close() noexcept
{
    closed_ = true;
    std::size_t discarded = 0;
    for (const auto& entry : receivers_) {
        discarded += entry.inbox->close();
    }
    return discarded;
}

} // namespace vaecu
