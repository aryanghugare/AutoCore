#pragma once

#include "vaecu/can_frame.hpp"

#include <cstddef>
#include <variant>

namespace vaecu {

enum class SendStatus {
    Routed,
    Closed,
    InvalidFrame
};

// Routed may have zero subscribers or partial delivery. For Routed, matching
// active subscribers equal delivered + dropped. Closed/InvalidFrame have zero counts.
struct SendResult {
    SendStatus status;
    std::size_t delivered;
    std::size_t dropped;
};

// These interfaces make no thread-safety guarantee. Callers must obey the
// concrete implementation's synchronization contract and preserve its lifetime.
class ICanSender {
public:
    virtual ~ICanSender() = default;

    ICanSender(const ICanSender&) = delete;
    ICanSender& operator=(const ICanSender&) = delete;
    ICanSender(ICanSender&&) = delete;
    ICanSender& operator=(ICanSender&&) = delete;

    // Does not wait for queue capacity. The implementation must copy any
    // retained frame; it cannot retain a reference to caller-owned storage.
    [[nodiscard]] virtual SendResult send(const CanFrame& frame) = 0;

protected:
    ICanSender() = default;
};

enum class ReceiveStatus {
    Empty,
    Closed
};

// A received frame is owned by the result. The alternative describes why
// no frame was returned; there is no separate, inconsistent "has frame" flag.
using ReceiveResult = std::variant<CanFrame, ReceiveStatus>;

// Shared ownership of a receiver does not synchronize its operations.
class ICanReceiver {
public:
    virtual ~ICanReceiver() = default;

    ICanReceiver(const ICanReceiver&) = delete;
    ICanReceiver& operator=(const ICanReceiver&) = delete;
    ICanReceiver(ICanReceiver&&) = delete;
    ICanReceiver& operator=(ICanReceiver&&) = delete;

    // Return an owned queued frame, Empty, or Closed without waiting.
    // Closed takes precedence over pending frames. A future concurrent
    // runtime can add a separate interruptible timed-wait operation.
    [[nodiscard]] virtual ReceiveResult try_receive() = 0;

protected:
    ICanReceiver() = default;
};

} // namespace vaecu
