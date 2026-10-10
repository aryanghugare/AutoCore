#include "vaecu/can_transport.hpp"
#include "vaecu/ecu.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <type_traits>
#include <utility>

namespace {

using vaecu::CanFrame;
using vaecu::CanId;
using vaecu::ICanReceiver;
using vaecu::ICanSender;
using vaecu::IEcu;
using vaecu::ReceiveResult;
using vaecu::ReceiveStatus;
using vaecu::SendResult;
using vaecu::SendStatus;
using vaecu::Timestamp;
using testing::Ref;
using testing::Return;
using testing::StrictMock;

static_assert(std::has_virtual_destructor_v<IEcu>);
static_assert(std::has_virtual_destructor_v<ICanSender>);
static_assert(std::has_virtual_destructor_v<ICanReceiver>);
static_assert(!std::is_copy_constructible_v<IEcu>);
static_assert(!std::is_copy_constructible_v<ICanSender>);
static_assert(!std::is_copy_constructible_v<ICanReceiver>);
static_assert(noexcept(std::declval<IEcu&>().stop()));

class MockSender : public ICanSender {
public:
    MOCK_METHOD(SendResult, send, (const CanFrame& frame), (override));
};

class MockReceiver : public ICanReceiver {
public:
    MOCK_METHOD(ReceiveResult, receive_until, (Timestamp deadline), (override));
};

class MockEcu : public IEcu {
public:
    MOCK_METHOD(void, start, (Timestamp now), (override));
    MOCK_METHOD(void, on_tick, (Timestamp now), (override));
    MOCK_METHOD(void, on_message, (const CanFrame& frame, Timestamp now), (override));
    MOCK_METHOD(void, stop, (), (noexcept, override));
};

TEST(TransportInterfaceTest, ReturnsPartialDeliveryWithoutLosingCounts)
{
    StrictMock<MockSender> sender;
    const CanFrame frame{CanId{0x100}, {42}, 1, Timestamp{}};
    EXPECT_CALL(sender, send(Ref(frame)))
        .WillOnce(Return(SendResult{SendStatus::Routed, 2, 1}));

    ICanSender& interface = sender;
    const SendResult result = interface.send(frame);

    EXPECT_EQ(result.status, SendStatus::Routed);
    EXPECT_EQ(result.delivered, 2U);
    EXPECT_EQ(result.dropped, 1U);
}

TEST(TransportInterfaceTest, DistinguishesNoSubscribersFromClosure)
{
    StrictMock<MockSender> sender;
    const CanFrame frame{CanId{0x100}, {}, 0, Timestamp{}};
    EXPECT_CALL(sender, send(Ref(frame)))
        .WillOnce(Return(SendResult{SendStatus::Routed, 0, 0}))
        .WillOnce(Return(SendResult{SendStatus::Closed, 0, 0}));

    ICanSender& interface = sender;
    EXPECT_EQ(interface.send(frame).status, SendStatus::Routed);
    EXPECT_EQ(interface.send(frame).status, SendStatus::Closed);
}

TEST(TransportInterfaceTest, ReturnsAnOwnedFrameUsingAnExplicitDeadline)
{
    StrictMock<MockReceiver> receiver;
    const Timestamp timestamp{std::chrono::milliseconds{100}};
    const Timestamp deadline{std::chrono::milliseconds{200}};
    const CanFrame frame{CanId{0x100}, {42}, 1, timestamp};
    EXPECT_CALL(receiver, receive_until(deadline))
        .WillOnce(Return(ReceiveResult{frame}));

    ICanReceiver& interface = receiver;
    const ReceiveResult result = interface.receive_until(deadline);

    ASSERT_TRUE(std::holds_alternative<CanFrame>(result));
    const CanFrame& received = std::get<CanFrame>(result);
    EXPECT_EQ(received.id(), frame.id());
    EXPECT_EQ(received.payload(), frame.payload());
    EXPECT_EQ(received.payload_size(), frame.payload_size());
    EXPECT_EQ(received.timestamp(), timestamp);
}

TEST(TransportInterfaceTest, DistinguishesTimeoutFromClosureWithoutAFrame)
{
    StrictMock<MockReceiver> receiver;
    const Timestamp deadline{std::chrono::milliseconds{200}};
    EXPECT_CALL(receiver, receive_until(deadline))
        .WillOnce(Return(ReceiveResult{ReceiveStatus::Timeout}))
        .WillOnce(Return(ReceiveResult{ReceiveStatus::Closed}));

    ICanReceiver& interface = receiver;
    const ReceiveResult timeout = interface.receive_until(deadline);
    const ReceiveResult closed = interface.receive_until(deadline);

    ASSERT_TRUE(std::holds_alternative<ReceiveStatus>(timeout));
    ASSERT_TRUE(std::holds_alternative<ReceiveStatus>(closed));
    EXPECT_EQ(std::get<ReceiveStatus>(timeout), ReceiveStatus::Timeout);
    EXPECT_EQ(std::get<ReceiveStatus>(closed), ReceiveStatus::Closed);
}

TEST(EcuInterfaceTest, SupportsExplicitLifecycleAndMessageCalls)
{
    auto ecu = std::make_unique<StrictMock<MockEcu>>();
    const Timestamp start_time{};
    const Timestamp tick_time{std::chrono::milliseconds{100}};
    const CanFrame frame{CanId{0x200}, {1}, 1, tick_time};
    {
        testing::InSequence sequence;
        EXPECT_CALL(*ecu, start(start_time));
        EXPECT_CALL(*ecu, on_tick(tick_time));
        EXPECT_CALL(*ecu, on_message(Ref(frame), tick_time));
        EXPECT_CALL(*ecu, stop()).Times(2);
    }

    std::unique_ptr<IEcu> interface = std::move(ecu);
    interface->start(start_time);
    interface->on_tick(tick_time);
    interface->on_message(frame, tick_time);
    interface->stop();
    interface->stop();
}

} // namespace
