#include "vaecu/demo_scenario.hpp"

#include "vaecu/brake_ecu.hpp"
#include "vaecu/engine_ecu.hpp"
#include "vaecu/virtual_can_bus.hpp"

#include <memory>
#include <stdexcept>
#include <utility>

namespace vaecu {
namespace {
using namespace std::chrono_literals;

struct Plan {
    std::chrono::milliseconds duration;
    bool sensor_failure;
    bool communication_timeout;
    bool recovery;
};

Plan plan_for(DemoScenario scenario)
{
    switch (scenario) {
    case DemoScenario::Normal: return {500ms, false, false, false};
    case DemoScenario::SensorFailure: return {500ms, true, false, false};
    case DemoScenario::CommunicationTimeout: return {1500ms, false, true, false};
    case DemoScenario::Recovery: return {1500ms, true, true, true};
    }
    throw std::invalid_argument("Unknown demonstration scenario");
}

// Private adapter records the real send outcome without changing the bus API.
class RecordingSender final : public ICanSender {
public:
    RecordingSender(VirtualCanBus& bus, DemoReport& report, DemoSource source, const DemoHooks& hooks)
        : bus_{bus}, report_{report}, source_{source}, hooks_{hooks} {}

    SendResult send(const CanFrame& frame) override
    {
        const auto sent = bus_.send(frame);
        report_.events.emplace_back(DemoTransmission{source_, frame, sent});
        if (hooks_.observe) hooks_.observe(report_.events.back());
        ++report_.summary.transmissions;
        report_.summary.admitted_copies += sent.delivered;
        report_.summary.dropped_copies += sent.dropped;
        if (sent.status != SendStatus::Routed || sent.delivered != 2 || sent.dropped != 0) {
            throw std::runtime_error("Demonstration frame admission failed");
        }
        return sent;
    }

private:
    VirtualCanBus& bus_;
    DemoReport& report_;
    DemoSource source_;
    const DemoHooks& hooks_;
};

class Session final {
public:
    explicit Session(DemoScenario scenario, DemoHooks hooks)
        : hooks_{std::move(hooks)}, report_{scenario, {}, {}}, engine_sender_{bus_, report_, DemoSource::Engine, hooks_},
          brake_sender_{bus_, report_, DemoSource::Brake, hooks_},
          scenario_sender_{bus_, report_, DemoSource::Scenario, hooks_},
          engine_{engine_sender_, {1500, 60}}, brake_{brake_sender_}, monitor_{store_}
    {
        report_.events.reserve(64); // All fixed plans fit without trace reallocation.
        commands_ = bus_.register_receiver("brake", {messages::brake_event_id});
        display_ = bus_.register_receiver("dashboard",
            {messages::engine_telemetry_id, messages::brake_status_id});
        diagnostic_ = bus_.register_receiver("diagnostics",
            {messages::engine_telemetry_id, messages::brake_status_id, messages::brake_event_id});
    }

    ~Session() { (void)cleanup(); }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    DemoReport run(Plan plan)
    {
        engine_.start(Timestamp{});
        brake_.start(Timestamp{});
        dashboard_.start(Timestamp{});
        monitor_.start(Timestamp{});
        action(Timestamp{}, DemoActionKind::Started);
        for (auto elapsed = 0ms; elapsed <= plan.duration; elapsed += 250ms) {
            if (hooks_.await_cycle && !hooks_.await_cycle(elapsed)) break;
            report_.summary.duration = elapsed;
            const Timestamp now{elapsed};
            if (elapsed == 250ms) {
                if (plan.sensor_failure) action(now, DemoActionKind::SensorFailed);
                if (plan.communication_timeout) action(now, DemoActionKind::EnginePaused);
            }
            if (plan.recovery && elapsed == 1250ms) {
                action(now, DemoActionKind::SensorRecovered);
                action(now, DemoActionKind::EngineResumed);
            }
            const bool recovered = plan.recovery && elapsed >= 1250ms;
            const bool bad_sensor = plan.sensor_failure && elapsed >= 250ms && !recovered;
            monitor_.observe_engine_inputs(bad_sensor ? -1 : 1500, 60, now);
            // A rejected raw sample never replaces the Engine's last valid input.
            if (!bad_sensor) engine_.set_inputs({1500, 60});
            if (elapsed == 250ms || elapsed == 500ms) {
                const auto event = elapsed == 250ms ? messages::BrakeEvent::Apply : messages::BrakeEvent::Release;
                (void)scenario_sender_.send(messages::encode_brake_event(event, now));
                dispatch(*commands_, brake_, now);
            }
            if (!plan.communication_timeout || elapsed == 0ms || recovered) engine_.on_tick(now);
            brake_.on_tick(now);
            dispatch(*display_, dashboard_, now);
            dispatch(*diagnostic_, monitor_, now);
            dashboard_.on_tick(now);
            monitor_.on_tick(now);
            drain_events();
            status(now, true);
        }
        for (const auto definition : fault_definitions) {
            const auto record = store_.record(definition.id);
            if (record && record->status == FaultStatus::Active) ++report_.summary.active_faults;
        }
        report_.summary.discarded_events = store_.discarded_events();
        report_.summary.pending_discarded = cleanup();
        const Timestamp stopped_at{report_.summary.duration};
        action(stopped_at, DemoActionKind::Stopped);
        status(stopped_at, false);
        return std::move(report_);
    }

private:
    std::size_t cleanup() noexcept
    {
        monitor_.stop();
        dashboard_.stop();
        brake_.stop();
        engine_.stop();
        const auto discarded = bus_.close();
        store_.close_events();
        return discarded;
    }

    void emit(DemoEvent event)
    {
        report_.events.push_back(std::move(event));
        if (hooks_.observe) hooks_.observe(report_.events.back());
    }

    void action(Timestamp now, DemoActionKind kind) { emit(DemoAction{now, kind}); }

    void dispatch(ICanReceiver& inbox, IEcu& ecu, Timestamp now)
    {
        for (;;) {
            const auto received = inbox.try_receive();
            const auto* frame = std::get_if<CanFrame>(&received);
            if (!frame) return;
            ecu.on_message(*frame, now);
            ++report_.summary.processed_copies;
        }
    }

    void drain_events()
    {
        while (const auto event = store_.take_event()) {
            emit(*event);
            if (event->kind == DiagnosticEventKind::Raised) ++report_.summary.faults_raised;
            else ++report_.summary.faults_recovered;
        }
    }

    bool timed_out(FaultId id) const
    {
        const auto record = store_.record(id);
        return record && record->status == FaultStatus::Active;
    }

    void status(Timestamp now, bool running)
    {
        const auto state = dashboard_.state();
        emit(DemoStatus{now,
            running && state.engine && !timed_out(FaultId::EngineCommunicationTimeout) ?
                EcuAvailability::Online : EcuAvailability::Offline,
            running && state.brake && !timed_out(FaultId::BrakeCommunicationTimeout) ?
                EcuAvailability::Online : EcuAvailability::Offline,
            running ? EcuAvailability::Online : EcuAvailability::Offline, state});
    }

    const DemoHooks hooks_;
    VirtualCanBus bus_;
    DemoReport report_;
    RecordingSender engine_sender_, brake_sender_, scenario_sender_;
    EngineEcu engine_;
    BrakeEcu brake_;
    DashboardEcu dashboard_;
    DiagnosticStore store_;
    DiagnosticMonitor monitor_;
    std::shared_ptr<CanReceiver> commands_, display_, diagnostic_;
};

} // namespace

DemoReport run_demo_scenario(DemoScenario scenario, DemoHooks hooks)
{
    const auto plan = plan_for(scenario); // Validate before creating a session.
    Session session{scenario, std::move(hooks)};
    return session.run(plan);
}

} // namespace vaecu
