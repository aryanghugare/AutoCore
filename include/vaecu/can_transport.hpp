#pragma once

#include "vaecu/can_frame.hpp"

#include <cstddef>
#include <variant>

namespace vaecu {

enum class SendStatus {
    Routed,
    Closed
};

// Routed may have zero subscribers or partial delivery. For Routed, matching
// subscribers equal delivered + dropped. Closed must have both counts zero.
struct SendResult {
    SendStatus status;
    std::size_t delivered;
    std::size_t dropped;
};

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
    Timeout,
    Closed
};

// A received frame is owned by the result. The alternative describes why
// no frame was returned; there is no separate, inconsistent "has frame" flag.
using ReceiveResult = std::variant<CanFrame, ReceiveStatus>;

class ICanReceiver {
public:
    virtual ~ICanReceiver() = default;

    ICanReceiver(const ICanReceiver&) = delete;
    ICanReceiver& operator=(const ICanReceiver&) = delete;
    ICanReceiver(ICanReceiver&&) = delete;
    ICanReceiver& operator=(ICanReceiver&&) = delete;

    // Return a queued frame, Timeout at the deadline, or Closed when the
    // receive endpoint closes. An already queued frame may be returned for
    // an expired deadline. Closed takes precedence over pending frames.
    // Waiting must be interruptible by closure; no polling/busy waiting.
    [[nodiscard]] virtual ReceiveResult receive_until(Timestamp deadline) = 0;

protected:
    ICanReceiver() = default;
};

} // namespace vaecu
