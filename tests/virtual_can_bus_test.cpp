#include "vaecu/virtual_can_bus.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <limits>
#include <memory>
#include <stdexcept>
#include <variant>

namespace {

using vaecu::CanFrame;
using vaecu::CanId;
using vaecu::CanReceiver;
using vaecu::ICanReceiver;
using vaecu::ICanSender;
using vaecu::ReceiveResult;
using vaecu::ReceiveStatus;
using vaecu::SendResult;
using vaecu::SendStatus;
using vaecu::Timestamp;
using vaecu::VirtualCanBus;

CanFrame make_frame(std::int64_t id, std::uint8_t marker = 42,
                    Timestamp timestamp = Timestamp{})
{
    return CanFrame{CanId{id}, {marker}, 1, timestamp};
}

void expect_result(const SendResult& result, SendStatus status,
                   std::size_t delivered, std::size_t dropped)
{
    EXPECT_EQ(result.status, status);
    EXPECT_EQ(result.delivered, delivered);
    EXPECT_EQ(result.dropped, dropped);
}

void expect_frame(ICanReceiver& receiver, const CanFrame& expected)
{
    const ReceiveResult result = receiver.try_receive();
    ASSERT_TRUE(std::holds_alternative<CanFrame>(result));
    const CanFrame& actual = std::get<CanFrame>(result);
    EXPECT_EQ(actual.id(), expected.id());
    EXPECT_EQ(actual.payload(), expected.payload());
    EXPECT_EQ(actual.payload_size(), expected.payload_size());
    EXPECT_EQ(actual.timestamp(), expected.timestamp());
}

void expect_status(ICanReceiver& receiver, ReceiveStatus expected)
{
    const ReceiveResult result = receiver.try_receive();
    ASSERT_TRUE(std::holds_alternative<ReceiveStatus>(result));
    EXPECT_EQ(std::get<ReceiveStatus>(result), expected);
}

TEST(BusRegistrationTest, RegistersAnInboxWithDefaultCapacity)
{
    VirtualCanBus bus;
    const auto inbox = bus.register_receiver("dashboard", {CanId{0x100}});

    EXPECT_EQ(inbox, bus.receiver("dashboard"));
    EXPECT_EQ(inbox->capacity(), 64U);
    EXPECT_EQ(inbox->pending(), 0U);
    EXPECT_FALSE(inbox->is_closed());
    expect_status(*inbox, ReceiveStatus::Empty);
}

TEST(BusRegistrationTest, RejectsAnEmptyName)
{
    VirtualCanBus bus;
    EXPECT_THROW((void)bus.register_receiver("", {CanId{0x100}}),
                 std::invalid_argument);
    EXPECT_EQ(bus.receiver(""), nullptr);
}

TEST(BusRegistrationTest, RejectsDuplicateNamesWithoutReplacingTheInbox)
{
    VirtualCanBus bus;
    const auto original = bus.register_receiver("engine", {CanId{0x200}});

    EXPECT_THROW((void)bus.register_receiver("engine", {CanId{0x100}}),
                 std::invalid_argument);
    EXPECT_EQ(bus.receiver("engine"), original);
}

TEST(BusRegistrationTest, RejectsZeroCapacityWithoutConsumingRegistrationSpace)
{
    VirtualCanBus bus{1};
    EXPECT_THROW((void)bus.register_receiver("engine", {CanId{0x200}}, 0),
                 std::invalid_argument);
    EXPECT_EQ(bus.receiver("engine"), nullptr);
    EXPECT_NO_THROW((void)bus.register_receiver("engine", {CanId{0x200}}, 1));
}

TEST(BusRegistrationTest, EnforcesTheReceiverLimit)
{
    VirtualCanBus bus{1};
    const auto first = bus.register_receiver("first", {});
    EXPECT_THROW((void)bus.register_receiver("second", {}), std::length_error);
    EXPECT_EQ(bus.receiver("second"), nullptr);
    EXPECT_EQ(bus.receiver("first"), first);
}

TEST(BusRegistrationTest, RejectsAZeroReceiverLimit)
{
    EXPECT_THROW((void)VirtualCanBus{0}, std::invalid_argument);
}

TEST(BusRegistrationTest, RejectsAnInvalidFilterIdentifierBeforeRegistration)
{
    VirtualCanBus bus;
    EXPECT_THROW((void)bus.register_receiver("bad", {CanId{0x800}}),
                 std::out_of_range);
    EXPECT_EQ(bus.receiver("bad"), nullptr);
}

TEST(BusRoutingTest, FansOutToEveryMatchingReceiverThroughTheSenderInterface)
{
    VirtualCanBus bus;
    const auto dashboard = bus.register_receiver("dashboard", {CanId{0x100}});
    const auto diagnostics = bus.register_receiver("diagnostics", {CanId{0x100}});
    const CanFrame frame = make_frame(0x100);
    ICanSender& sender = bus;

    expect_result(sender.send(frame), SendStatus::Routed, 2, 0);
    expect_frame(*dashboard, frame);
    expect_frame(*diagnostics, frame);
}

TEST(BusRoutingTest, FiltersByExactMessageIdentifier)
{
    VirtualCanBus bus;
    const auto engine = bus.register_receiver("engine", {CanId{0x200}});
    const auto dashboard = bus.register_receiver("dashboard", {CanId{0x100}});
    const CanFrame frame = make_frame(0x100);

    expect_result(bus.send(frame), SendStatus::Routed, 1, 0);
    expect_status(*engine, ReceiveStatus::Empty);
    expect_frame(*dashboard, frame);
}

TEST(BusRoutingTest, AnEmptyFilterAcceptsNothing)
{
    VirtualCanBus bus;
    const auto brake = bus.register_receiver("brake", {});

    expect_result(bus.send(make_frame(0x100)), SendStatus::Routed, 0, 0);
    expect_status(*brake, ReceiveStatus::Empty);
}

TEST(BusRoutingTest, DuplicateFilterIdentifiersDoNotDuplicateDelivery)
{
    VirtualCanBus bus;
    const auto inbox = bus.register_receiver("dashboard", {CanId{0x100}, CanId{0x100}});
    const CanFrame frame = make_frame(0x100);

    expect_result(bus.send(frame), SendStatus::Routed, 1, 0);
    expect_frame(*inbox, frame);
    expect_status(*inbox, ReceiveStatus::Empty);
}

TEST(BusRoutingTest, SupportsTheLowestAndHighestIdentifiers)
{
    VirtualCanBus bus;
    const auto inbox = bus.register_receiver("monitor", {CanId{0}, CanId{0x7FF}});
    const CanFrame low = make_frame(0);
    const CanFrame high = make_frame(0x7FF);

    expect_result(bus.send(low), SendStatus::Routed, 1, 0);
    expect_result(bus.send(high), SendStatus::Routed, 1, 0);
    expect_frame(*inbox, low);
    expect_frame(*inbox, high);
}

TEST(BusRoutingTest, AnUnmatchedIdentifierDoesNotCreateOrOverflowAnInbox)
{
    VirtualCanBus bus;
    const auto inbox = bus.register_receiver("engine", {CanId{0x200}}, 1);
    const CanFrame accepted = make_frame(0x200);
    expect_result(bus.send(accepted), SendStatus::Routed, 1, 0);

    expect_result(bus.send(make_frame(0x123)), SendStatus::Routed, 0, 0);
    EXPECT_EQ(inbox->pending(), 1U);
    expect_frame(*inbox, accepted);
}

TEST(BusRoutingTest, UnknownReceiversReturnNoHandleAndCannotBeStopped)
{
    VirtualCanBus bus;
    const auto inbox = bus.register_receiver("engine", {CanId{0x200}});

    EXPECT_EQ(bus.receiver("unknown"), nullptr);
    EXPECT_FALSE(bus.stop_receiver("unknown"));
    EXPECT_FALSE(inbox->is_closed());
    expect_result(bus.send(make_frame(0x200)), SendStatus::Routed, 1, 0);
}

TEST(BusRoutingTest, ANewReceiverDoesNotReplayEarlierPublications)
{
    VirtualCanBus bus;
    expect_result(bus.send(make_frame(0x100)), SendStatus::Routed, 0, 0);
    const auto inbox = bus.register_receiver("dashboard", {CanId{0x100}});

    expect_status(*inbox, ReceiveStatus::Empty);
    expect_result(bus.send(make_frame(0x100)), SendStatus::Routed, 1, 0);
}

TEST(BusRoutingTest, OwnsQueuedFramesWhenTheCallerReassignsItsFrame)
{
    VirtualCanBus bus;
    const auto inbox = bus.register_receiver("dashboard", {CanId{0x100}});
    CanFrame frame = make_frame(0x100, 1);
    const CanFrame expected = frame;
    expect_result(bus.send(frame), SendStatus::Routed, 1, 0);
    frame = make_frame(0x200, 99);

    expect_frame(*inbox, expected);
}

TEST(BusRoutingTest, DeliveriesToReceiversAreConsumedIndependently)
{
    VirtualCanBus bus;
    const auto first = bus.register_receiver("first", {CanId{0x100}});
    const auto second = bus.register_receiver("second", {CanId{0x100}});
    const CanFrame frame = make_frame(0x100);
    expect_result(bus.send(frame), SendStatus::Routed, 2, 0);

    expect_frame(*first, frame);
    EXPECT_EQ(first->pending(), 0U);
    EXPECT_EQ(second->pending(), 1U);
    expect_frame(*second, frame);
}

TEST(BusQueueTest, UsesPublicationOrderInsteadOfIdentifierOrTimestampPriority)
{
    VirtualCanBus bus;
    const auto inbox = bus.register_receiver("monitor", {CanId{0x700}, CanId{0x100}});
    const CanFrame first = make_frame(0x700, 1, Timestamp{std::chrono::milliseconds{200}});
    const CanFrame second = make_frame(0x100, 2, Timestamp{std::chrono::milliseconds{100}});

    expect_result(bus.send(first), SendStatus::Routed, 1, 0);
    expect_result(bus.send(second), SendStatus::Routed, 1, 0);
    expect_frame(*inbox, first);
    expect_frame(*inbox, second);
}

TEST(BusQueueTest, RejectsTheNewestFrameAndPreservesTheExistingQueue)
{
    VirtualCanBus bus;
    const auto inbox = bus.register_receiver("monitor", {CanId{0x100}}, 2);
    const CanFrame first = make_frame(0x100, 1);
    const CanFrame second = make_frame(0x100, 2);

    expect_result(bus.send(first), SendStatus::Routed, 1, 0);
    expect_result(bus.send(second), SendStatus::Routed, 1, 0);
    expect_result(bus.send(make_frame(0x100, 3)), SendStatus::Routed, 0, 1);
    EXPECT_EQ(inbox->pending(), 2U);
    expect_frame(*inbox, first);
    expect_frame(*inbox, second);
    expect_status(*inbox, ReceiveStatus::Empty);
}

TEST(BusQueueTest, FillsExactlyTheDefaultCapacityBeforeDropping)
{
    VirtualCanBus bus;
    const auto inbox = bus.register_receiver("monitor", {CanId{0x100}});
    const CanFrame frame = make_frame(0x100);

    for (std::size_t index = 0; index < CanReceiver::default_capacity; ++index) {
        expect_result(bus.send(frame), SendStatus::Routed, 1, 0);
    }
    expect_result(bus.send(frame), SendStatus::Routed, 0, 1);
    EXPECT_EQ(inbox->pending(), CanReceiver::default_capacity);

    for (std::size_t index = 0; index < CanReceiver::default_capacity; ++index) {
        SCOPED_TRACE(index);
        expect_frame(*inbox, frame);
    }
    EXPECT_EQ(inbox->pending(), 0U);
    expect_status(*inbox, ReceiveStatus::Empty);
}

TEST(BusQueueTest, AnOverflowingReceiverDoesNotBlockAnotherReceiver)
{
    VirtualCanBus bus;
    const auto full = bus.register_receiver("full", {CanId{0x100}}, 1);
    const auto healthy = bus.register_receiver("healthy", {CanId{0x100}}, 1);
    const CanFrame first = make_frame(0x100, 1);
    const CanFrame second = make_frame(0x100, 2);
    expect_result(bus.send(first), SendStatus::Routed, 2, 0);
    expect_frame(*healthy, first);

    expect_result(bus.send(second), SendStatus::Routed, 1, 1);
    expect_frame(*full, first);
    expect_frame(*healthy, second);
}

TEST(BusQueueTest, ReportsAllMatchingReceiversAsDroppedWhenAllAreFull)
{
    VirtualCanBus bus;
    const auto first = bus.register_receiver("first", {CanId{0x100}}, 1);
    const auto second = bus.register_receiver("second", {CanId{0x100}}, 1);
    const CanFrame frame = make_frame(0x100);
    expect_result(bus.send(frame), SendStatus::Routed, 2, 0);

    expect_result(bus.send(frame), SendStatus::Routed, 0, 2);
    EXPECT_EQ(first->pending(), 1U);
    EXPECT_EQ(second->pending(), 1U);
}

TEST(BusQueueTest, ReusesFreedSlotsAndPreservesOrderAcrossWraparound)
{
    VirtualCanBus bus;
    const auto inbox = bus.register_receiver("monitor", {CanId{0x100}}, 2);
    const CanFrame first = make_frame(0x100, 1);
    const CanFrame second = make_frame(0x100, 2);
    const CanFrame third = make_frame(0x100, 3);

    for (std::size_t cycle = 0; cycle < 10; ++cycle) {
        SCOPED_TRACE(cycle);
        expect_result(bus.send(first), SendStatus::Routed, 1, 0);
        expect_result(bus.send(second), SendStatus::Routed, 1, 0);
        expect_frame(*inbox, first);
        expect_result(bus.send(third), SendStatus::Routed, 1, 0);
        expect_frame(*inbox, second);
        expect_frame(*inbox, third);
        expect_status(*inbox, ReceiveStatus::Empty);
    }
}

TEST(BusValidationTest, RejectsInvalidRawIdentifiersWithoutChangingQueues)
{
    VirtualCanBus bus;
    const auto inbox = bus.register_receiver("monitor", {CanId{0x100}});
    const CanFrame existing = make_frame(0x100);
    expect_result(bus.send(existing), SendStatus::Routed, 1, 0);
    const std::array<std::int64_t, 5> invalid{
        -1, 0x800, 0x10000, std::numeric_limits<std::int64_t>::min(),
        std::numeric_limits<std::int64_t>::max()};

    for (const std::int64_t id : invalid) {
        SCOPED_TRACE(id);
        expect_result(bus.send_raw(id, {}, 0, Timestamp{}), SendStatus::InvalidFrame, 0, 0);
        EXPECT_EQ(inbox->pending(), 1U);
    }
    expect_frame(*inbox, existing);
}

TEST(BusValidationTest, RejectsInvalidRawPayloadSizesWithoutChangingQueues)
{
    VirtualCanBus bus;
    const auto inbox = bus.register_receiver("monitor", {CanId{0x100}}, 1);
    const CanFrame existing = make_frame(0x100, 7);
    expect_result(bus.send(existing), SendStatus::Routed, 1, 0);
    const std::array<std::size_t, 2> invalid{9, std::numeric_limits<std::size_t>::max()};

    for (const std::size_t size : invalid) {
        SCOPED_TRACE(size);
        expect_result(bus.send_raw(0x100, {}, size, Timestamp{}),
                      SendStatus::InvalidFrame, 0, 0);
        EXPECT_EQ(inbox->pending(), 1U);
    }
    expect_frame(*inbox, existing);
    expect_status(*inbox, ReceiveStatus::Empty);
}

TEST(BusValidationTest, AcceptsRawEnvelopeBoundariesAndPreservesContents)
{
    VirtualCanBus bus;
    const auto inbox = bus.register_receiver("monitor", {CanId{0}, CanId{0x7FF}});
    const Timestamp timestamp{std::chrono::milliseconds{123}};
    const CanFrame::Payload payload{0, 1, 2, 3, 4, 5, 6, 255};

    expect_result(bus.send_raw(0, payload, 0, timestamp), SendStatus::Routed, 1, 0);
    expect_result(bus.send_raw(0x7FF, payload, 8, timestamp), SendStatus::Routed, 1, 0);
    expect_frame(*inbox, CanFrame{CanId{0}, payload, 0, timestamp});
    expect_frame(*inbox, CanFrame{CanId{0x7FF}, payload, 8, timestamp});
}

TEST(BusValidationTest, LeavesApplicationPayloadValidationToFutureCodecs)
{
    VirtualCanBus bus;
    const auto inbox = bus.register_receiver("monitor", {CanId{0x100}});
    const CanFrame frame{CanId{0x100}, {255, 255, 255, 255, 255, 255, 255, 255},
                         8, Timestamp{}};

    expect_result(bus.send(frame), SendStatus::Routed, 1, 0);
    expect_frame(*inbox, frame);
}

TEST(BusLifecycleTest, StoppingAReceiverDiscardsPendingFramesAndExcludesNewTraffic)
{
    VirtualCanBus bus;
    const auto stopped = bus.register_receiver("stopped", {CanId{0x100}});
    const auto active = bus.register_receiver("active", {CanId{0x100}});
    const CanFrame frame = make_frame(0x100);
    expect_result(bus.send(frame), SendStatus::Routed, 2, 0);

    EXPECT_TRUE(bus.stop_receiver("stopped"));
    EXPECT_TRUE(bus.stop_receiver("stopped"));
    EXPECT_EQ(stopped->pending(), 0U);
    expect_status(*stopped, ReceiveStatus::Closed);
    expect_result(bus.send(frame), SendStatus::Routed, 1, 0);
    EXPECT_EQ(active->pending(), 2U);
}

TEST(BusLifecycleTest, ClosingAnInboxReportsDiscardedFramesAndIsIdempotent)
{
    VirtualCanBus bus;
    const auto inbox = bus.register_receiver("monitor", {CanId{0x100}});
    expect_result(bus.send(make_frame(0x100)), SendStatus::Routed, 1, 0);

    EXPECT_EQ(inbox->close(), 1U);
    EXPECT_EQ(inbox->close(), 0U);
    expect_status(*inbox, ReceiveStatus::Closed);
}

TEST(BusLifecycleTest, ClosingTheBusDiscardsEveryPendingDeliveryAndIsIdempotent)
{
    VirtualCanBus bus;
    const auto first = bus.register_receiver("first", {CanId{0x100}});
    const auto second = bus.register_receiver("second", {CanId{0x100}});
    expect_result(bus.send(make_frame(0x100)), SendStatus::Routed, 2, 0);

    EXPECT_EQ(bus.close(), 2U);
    EXPECT_EQ(bus.close(), 0U);
    EXPECT_TRUE(bus.is_closed());
    expect_status(*first, ReceiveStatus::Closed);
    expect_status(*second, ReceiveStatus::Closed);
}

TEST(BusLifecycleTest, AClosedBusRejectsTypedAndRawPublications)
{
    VirtualCanBus bus;
    (void)bus.close();

    expect_result(bus.send(make_frame(0x100)), SendStatus::Closed, 0, 0);
    expect_result(bus.send_raw(0x100, {}, 0, Timestamp{}), SendStatus::Closed, 0, 0);
    expect_result(bus.send_raw(-1, {}, 9, Timestamp{}), SendStatus::Closed, 0, 0);
}

TEST(BusLifecycleTest, AClosedBusRejectsRegistration)
{
    VirtualCanBus bus;
    (void)bus.close();
    EXPECT_THROW((void)bus.register_receiver("new", {}), std::logic_error);
}

TEST(BusLifecycleTest, ReceiverHandlesRemainSafeAfterBusDestruction)
{
    std::shared_ptr<ICanReceiver> receiver;
    {
        VirtualCanBus bus;
        receiver = bus.register_receiver("monitor", {CanId{0x100}});
        expect_result(bus.send(make_frame(0x100)), SendStatus::Routed, 1, 0);
    }

    ASSERT_NE(receiver, nullptr);
    expect_status(*receiver, ReceiveStatus::Closed);
}

TEST(BusLifecycleTest, StoppedRegistrationsCannotBeReplacedImplicitly)
{
    VirtualCanBus bus;
    const auto inbox = bus.register_receiver("engine", {CanId{0x200}});
    EXPECT_TRUE(bus.stop_receiver("engine"));

    EXPECT_THROW((void)bus.register_receiver("engine", {CanId{0x100}}),
                 std::invalid_argument);
    EXPECT_EQ(bus.receiver("engine"), inbox);
}

} // namespace
