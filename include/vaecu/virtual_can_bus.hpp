#pragma once

#include "vaecu/can_transport.hpp"

#include <bitset>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace vaecu {

class VirtualCanBus;

// One receiver's immutable filter and bounded, thread-safe FIFO.
class CanReceiver final : public ICanReceiver {
public:
    static constexpr std::size_t default_capacity = 64;

    explicit CanReceiver(std::initializer_list<CanId> accepted_ids,
                         std::size_t capacity = default_capacity);

    [[nodiscard]] ReceiveResult try_receive() noexcept override;
    // Predicate wait: Empty means deadline elapsed; close wakes all waiters
    // and discards queued frames. A queued frame wins over an elapsed deadline.
    [[nodiscard]] ReceiveResult receive_until(Timestamp deadline);
    [[nodiscard]] std::size_t capacity() const noexcept;
    [[nodiscard]] std::size_t pending() const noexcept;
    [[nodiscard]] bool is_closed() const noexcept;

    // Discard pending frames and return the count. Repeated close returns zero.
    [[nodiscard]] std::size_t close() noexcept;

private:
    friend class VirtualCanBus;

    enum class Admission { Delivered, Full, Excluded };
    [[nodiscard]] Admission enqueue(const CanFrame& frame) noexcept;
    [[nodiscard]] ReceiveResult pop_locked() noexcept;

    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::bitset<static_cast<std::size_t>(CanId::max_value) + 1> filter_;
    std::vector<std::optional<CanFrame>> slots_;
    std::size_t head_{0};
    std::size_t size_{0};
    bool closed_{false};
};

// Thread-safe synchronous fan-out. Sends are serialized, with bus-before-inbox
// lock order. No callbacks under locks, capacity waits, or arbitration.
class VirtualCanBus final : public ICanSender {
public:
    explicit VirtualCanBus(std::size_t max_receivers = 4);
    ~VirtualCanBus() override;

    // Names must be nonempty and unique; capacity must be positive. Filters
    // are exact-ID sets, with an empty set matching nothing. Throws for invalid
    // configuration, exhausted registration capacity, or a closed bus.
    [[nodiscard]] std::shared_ptr<CanReceiver> register_receiver(
        const std::string& name, std::initializer_list<CanId> accepted_ids,
        std::size_t capacity = CanReceiver::default_capacity);

    // An unknown name returns nullptr / false and never creates a receiver.
    [[nodiscard]] std::shared_ptr<CanReceiver> receiver(const std::string& name) const;
    [[nodiscard]] bool stop_receiver(const std::string& name) noexcept;

    [[nodiscard]] SendResult send(const CanFrame& frame) noexcept override;

    // Raw envelope boundary: validate before constructing a CanFrame. This
    // does not validate application-level signal encoding.
    [[nodiscard]] SendResult send_raw(std::int64_t identifier,
        const CanFrame::Payload& payload, std::size_t payload_size,
        Timestamp timestamp);

    [[nodiscard]] bool is_closed() const noexcept;
    [[nodiscard]] std::size_t close() noexcept;

private:
    [[nodiscard]] SendResult route_locked(const CanFrame& frame) noexcept;
    struct Registration {
        std::string name;
        std::shared_ptr<CanReceiver> inbox;
    };

    mutable std::mutex mutex_;
    std::size_t max_receivers_;
    std::vector<Registration> receivers_;
    bool closed_{false};
};

} // namespace vaecu
