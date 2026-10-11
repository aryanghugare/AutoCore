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

CanReceiver::Admission CanReceiver::enqueue(const CanFrame& frame) noexcept
{
    {
        const std::lock_guard<std::mutex> lock{mutex_};
        if (closed_ || !filter_[frame.id().value()]) return Admission::Excluded;
        if (size_ == slots_.size()) return Admission::Full;
        const std::size_t tail = (head_ + size_) % slots_.size();
        slots_[tail].emplace(frame);
        ++size_;
    }
    ready_.notify_one();
    return Admission::Delivered;
}

ReceiveResult CanReceiver::try_receive() noexcept
{
    const std::lock_guard<std::mutex> lock{mutex_};
    return pop_locked();
}

ReceiveResult CanReceiver::receive_until(Timestamp deadline)
{
    std::unique_lock<std::mutex> lock{mutex_};
    if (deadline > Timestamp::clock::now()) {
        ready_.wait_until(lock, deadline, [this] { return closed_ || size_ != 0; });
    }
    return pop_locked();
}

ReceiveResult CanReceiver::pop_locked() noexcept
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
    const std::lock_guard<std::mutex> lock{mutex_};
    return size_;
}

bool CanReceiver::is_closed() const noexcept
{
    const std::lock_guard<std::mutex> lock{mutex_};
    return closed_;
}

std::size_t CanReceiver::close() noexcept
{
    std::size_t discarded;
    {
        const std::lock_guard<std::mutex> lock{mutex_};
        discarded = size_;
        closed_ = true;
        for (auto& slot : slots_) slot.reset();
        head_ = 0;
        size_ = 0;
    }
    ready_.notify_all();
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
    const std::lock_guard<std::mutex> lock{mutex_};
    if (closed_) {
        throw std::logic_error("Cannot register a receiver on a closed bus");
    }
    bool duplicate = false;
    for (const auto& entry : receivers_) duplicate = duplicate || entry.name == name;
    if (name.empty() || duplicate) {
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
    const std::lock_guard<std::mutex> lock{mutex_};
    for (const auto& entry : receivers_) {
        if (entry.name == name) {
            return entry.inbox;
        }
    }
    return {};
}

bool VirtualCanBus::stop_receiver(const std::string& name) noexcept
{
    const std::lock_guard<std::mutex> lock{mutex_};
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
    const std::lock_guard<std::mutex> lock{mutex_};
    return route_locked(frame);
}

SendResult VirtualCanBus::route_locked(const CanFrame& frame) noexcept
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
        const auto admission = entry.inbox->enqueue(frame);
        if (admission == CanReceiver::Admission::Delivered) {
            ++result.delivered;
        } else if (admission == CanReceiver::Admission::Full) {
            ++result.dropped;
        }
    }
    return result;
}

SendResult VirtualCanBus::send_raw(std::int64_t identifier,
    const CanFrame::Payload& payload, std::size_t payload_size, Timestamp timestamp)
{
    const std::lock_guard<std::mutex> lock{mutex_};
    if (closed_) {
        return {SendStatus::Closed, 0, 0};
    }
    if (!CanId::is_valid(identifier) ||
        !CanFrame::is_valid_payload_size(payload_size)) {
        return {SendStatus::InvalidFrame, 0, 0};
    }
    return route_locked(CanFrame{CanId{identifier}, payload, payload_size, timestamp});
}

bool VirtualCanBus::is_closed() const noexcept
{
    const std::lock_guard<std::mutex> lock{mutex_};
    return closed_;
}

std::size_t VirtualCanBus::close() noexcept
{
    const std::lock_guard<std::mutex> lock{mutex_};
    closed_ = true;
    std::size_t discarded = 0;
    for (const auto& entry : receivers_) {
        discarded += entry.inbox->close();
    }
    return discarded;
}

} // namespace vaecu
