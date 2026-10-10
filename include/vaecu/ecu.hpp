#pragma once

#include "vaecu/can_frame.hpp"

namespace vaecu {

// Observable outcome of the latest message callback; not a diagnostic DTC.
enum class MessageDisposition {
    None, Accepted, UnsupportedId, InvalidPayload, InvalidTimestamp, OutOfOrder
};

// Behavior interface only. A future runtime owns and serializes these calls;
// implementations do not create threads, wait for frames, or join workers.
class IEcu {
public:
    virtual ~IEcu() = default;

    IEcu(const IEcu&) = delete;
    IEcu& operator=(const IEcu&) = delete;
    IEcu(IEcu&&) = delete;
    IEcu& operator=(IEcu&&) = delete;

    // Start once per instance. No tick/message calls before start or after
    // stop. Stop is valid before start or after a failed start and can be
    // called repeatedly. Copy a frame if retaining it beyond on_message().
    // Processing 'now' must be nondecreasing from start across all callbacks;
    // a frame's publication timestamp may be earlier than processing time.
    virtual void start(Timestamp now) = 0;
    virtual void on_tick(Timestamp now) = 0;
    virtual void on_message(const CanFrame& frame, Timestamp now) = 0;
    virtual void stop() noexcept = 0;

protected:
    IEcu() = default;
};

} // namespace vaecu
