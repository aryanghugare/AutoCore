#pragma once

#include "vaecu/can_frame.hpp"

namespace vaecu {

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
    virtual void start(Timestamp now) = 0;
    virtual void on_tick(Timestamp now) = 0;
    virtual void on_message(const CanFrame& frame, Timestamp now) = 0;
    virtual void stop() noexcept = 0;

protected:
    IEcu() = default;
};

} // namespace vaecu
