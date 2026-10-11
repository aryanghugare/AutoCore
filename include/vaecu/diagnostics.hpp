#pragma once

#include "vaecu/ecu.hpp"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <mutex>
#include <string_view>
#include <vector>

namespace vaecu {

// Project-local fault identifiers; not standardized OBD codes or UDS services.
enum class FaultId : std::uint16_t {
    EngineSpeedSensorInvalid = 0x1001,
    VehicleSpeedSensorInvalid = 0x1002,
    EngineMessageMissing = 0x1101,
    EngineMessageStale = 0x1102,
    EnginePayloadInvalid = 0x1103,
    EngineCommunicationTimeout = 0x1104,
    BrakeMessageMissing = 0x1201,
    BrakeMessageStale = 0x1202,
    BrakePayloadInvalid = 0x1203,
    BrakeCommunicationTimeout = 0x1204,
    BrakeEventPayloadInvalid = 0x1205
};

struct FaultDefinition {
    FaultId id;
    std::string_view name;
};

inline constexpr std::array<FaultDefinition, 11> fault_definitions{{
    {FaultId::EngineSpeedSensorInvalid, "engine speed sensor invalid"},
    {FaultId::VehicleSpeedSensorInvalid, "vehicle speed sensor invalid"},
    {FaultId::EngineMessageMissing, "engine message missing"},
    {FaultId::EngineMessageStale, "engine message stale"},
    {FaultId::EnginePayloadInvalid, "engine payload invalid"},
    {FaultId::EngineCommunicationTimeout, "engine communication timeout"},
    {FaultId::BrakeMessageMissing, "brake message missing"},
    {FaultId::BrakeMessageStale, "brake message stale"},
    {FaultId::BrakePayloadInvalid, "brake payload invalid"},
    {FaultId::BrakeCommunicationTimeout, "brake communication timeout"},
    {FaultId::BrakeEventPayloadInvalid, "brake event payload invalid"}
}};

enum class FaultStatus { Active, Cleared };
enum class DiagnosticEventKind { Raised, Recovered };

struct DiagnosticRecord {
    FaultId id;
    FaultStatus status;
    Timestamp first_occurrence;
    Timestamp last_occurrence;
    std::optional<Timestamp> last_recovery;
    std::size_t occurrence_count;
};

struct DiagnosticEvent {
    FaultId id;
    DiagnosticEventKind kind;
    Timestamp timestamp;
};

// A monitor owns fault-state decisions for its sink during one session. Its
// assigned faults must initially be inactive, with no other writer for those
// faults during observation.
// Calls are serialized, with nondecreasing observation timestamps. Sink must
// outlive its monitor. It must not call back into the monitor.
class IDiagnosticSink {
public:
    virtual ~IDiagnosticSink() = default;
    IDiagnosticSink(const IDiagnosticSink&) = delete;
    IDiagnosticSink& operator=(const IDiagnosticSink&) = delete;
    IDiagnosticSink(IDiagnosticSink&&) = delete;
    IDiagnosticSink& operator=(IDiagnosticSink&&) = delete;
    virtual void set_fault(FaultId id, bool active, Timestamp now) = 0;

protected:
    IDiagnosticSink() = default;
};

// Thread-safe storage: one retained record per known fault and bounded FIFO.
// Overflow overwrites the oldest event and increments discarded_events().
class DiagnosticStore final : public IDiagnosticSink {
public:
    explicit DiagnosticStore(std::size_t event_capacity = 64);
    void set_fault(FaultId id, bool active, Timestamp now) override;
    [[nodiscard]] std::optional<DiagnosticRecord> record(FaultId id) const;
    [[nodiscard]] std::optional<DiagnosticEvent> take_event() noexcept;
    // Wait for an event; nullopt means deadline elapsed or the closed feed is
    // empty. Closing retains pending events and records but rejects updates.
    [[nodiscard]] std::optional<DiagnosticEvent> wait_event_until(Timestamp deadline);
    void close_events() noexcept;
    [[nodiscard]] bool events_closed() const noexcept;
    [[nodiscard]] std::size_t pending_events() const noexcept;
    [[nodiscard]] std::size_t discarded_events() const noexcept;

private:
    void append_event(DiagnosticEvent event) noexcept;
    [[nodiscard]] std::optional<DiagnosticEvent> pop_event_locked() noexcept;
    mutable std::mutex mutex_;
    std::condition_variable event_ready_;
    std::array<std::optional<DiagnosticRecord>, fault_definitions.size()> records_{};
    std::vector<std::optional<DiagnosticEvent>> events_;
    std::optional<Timestamp> last_now_;
    std::size_t head_{0};
    std::size_t size_{0};
    std::size_t discarded_{0};
    bool events_closed_{false};
};

struct DiagnosticConfig {
    Timestamp::duration startup_grace{std::chrono::milliseconds{500}};
    Timestamp::duration stale_after{std::chrono::milliseconds{500}};
    Timestamp::duration communication_timeout{std::chrono::milliseconds{1000}};
};

// Detection only: caller dispatches subscribed frames and ticks, even during
// silence. All time is injected through callbacks; no threads or clock reads.
class DiagnosticMonitor final : public IEcu {
public:
    explicit DiagnosticMonitor(IDiagnosticSink& sink, DiagnosticConfig config = {});
    void start(Timestamp now) override;
    void on_tick(Timestamp now) override;
    void on_message(const CanFrame& frame, Timestamp now) override;
    void stop() noexcept override;

    // Scenario observes signed raw samples BEFORE attempting EngineEcu input
    // validation. Good telemetry does not clear a bad raw-sensor observation.
    void observe_engine_inputs(std::int64_t engine_rpm, std::int64_t vehicle_speed_kph,
                               Timestamp now);

private:
    struct Stream {
        std::optional<Timestamp> publication;
        std::optional<Timestamp> received;
    };
    void advance(Timestamp now);
    void emit(FaultId id, bool active, Timestamp now);
    void observe_stream(std::size_t index, bool valid_payload,
                        const CanFrame& frame, Timestamp now);
    void evaluate(Timestamp now);
    IDiagnosticSink& sink_;
    DiagnosticConfig config_;
    std::array<Stream, 3> streams_{};
    std::array<bool, fault_definitions.size()> active_{};
    Timestamp started_at_{};
    Timestamp last_now_{};
    bool running_{false};
    bool stopped_{false};
};

} // namespace vaecu
