#include "vaecu/can_frame.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace {

using vaecu::CanFrame;
using vaecu::CanId;
using vaecu::Timestamp;

static_assert(!std::is_convertible_v<int, CanId>);
static_assert(!std::is_convertible_v<CanId, std::uint16_t>);
static_assert(!std::is_default_constructible_v<CanId>);
static_assert(!std::is_default_constructible_v<CanFrame>);
static_assert(std::is_nothrow_copy_constructible_v<CanFrame>);
static_assert(std::is_nothrow_move_constructible_v<CanFrame>);
static_assert(std::is_same_v<decltype(std::declval<const CanFrame&>().payload()),
                             const CanFrame::Payload&>);

TEST(CanIdTest, AcceptsLowerAndUpperBoundaries)
{
    EXPECT_EQ(CanId{0}.value(), 0);
    EXPECT_EQ(CanId{0x7FF}.value(), 0x7FF);
}

TEST(CanIdTest, AcceptsNormalIdentifier)
{
    EXPECT_EQ(CanId{0x100}.value(), 0x100);
}

TEST(CanIdTest, RejectsNegativeIdentifiers)
{
    EXPECT_THROW((void)CanId{-1}, std::out_of_range);
    EXPECT_THROW((void)CanId{std::numeric_limits<std::int64_t>::min()},
                 std::out_of_range);
}

TEST(CanIdTest, RejectsIdentifiersAboveElevenBitsWithoutTruncation)
{
    EXPECT_THROW((void)CanId{0x800}, std::out_of_range);
    EXPECT_THROW((void)CanId{0x10000}, std::out_of_range);
    EXPECT_THROW((void)CanId{std::numeric_limits<std::int64_t>::max()},
                 std::out_of_range);
}

TEST(CanIdTest, SupportsNonThrowingValidation)
{
    EXPECT_TRUE(CanId::is_valid(0));
    EXPECT_TRUE(CanId::is_valid(0x7FF));
    EXPECT_FALSE(CanId::is_valid(-1));
    EXPECT_FALSE(CanId::is_valid(0x800));
}

TEST(CanIdTest, SupportsValueComparisonAndCopy)
{
    const CanId first{0x100};
    const CanId copy{first};
    const CanId other{0x200};

    EXPECT_EQ(first, copy);
    EXPECT_NE(first, other);
    EXPECT_LT(first, other);
}

TEST(CanFrameTest, AcceptsAllPayloadLengthsFromZeroThroughEight)
{
    const CanFrame::Payload payload{0, 1, 2, 3, 4, 5, 6, 255};

    for (std::size_t size = 0; size <= CanFrame::max_payload_size; ++size) {
        SCOPED_TRACE(size);
        const CanFrame frame{CanId{0x100}, payload, size, Timestamp{}};
        EXPECT_EQ(frame.payload_size(), size);
    }
}

TEST(CanFrameTest, PreservesEightBytePayloadAndExplicitTimestamp)
{
    const CanFrame::Payload payload{0, 1, 2, 3, 4, 5, 6, 255};
    const Timestamp timestamp{std::chrono::milliseconds{123}};
    const CanFrame frame{CanId{0x7FF}, payload, payload.size(), timestamp};

    EXPECT_EQ(frame.id(), CanId{0x7FF});
    EXPECT_EQ(frame.payload(), payload);
    EXPECT_EQ(frame.payload_size(), 8U);
    EXPECT_EQ(frame.timestamp(), timestamp);
}

TEST(CanFrameTest, ZeroLengthPayloadClearsAllUnusedStorage)
{
    const CanFrame::Payload payload{255, 255, 255, 255, 255, 255, 255, 255};
    const CanFrame frame{CanId{0}, payload, 0, Timestamp{}};

    EXPECT_EQ(frame.payload_size(), 0U);
    EXPECT_EQ(frame.payload(), CanFrame::Payload{});
}

TEST(CanFrameTest, ClearsOnlyBytesOutsideTheActivePayload)
{
    const CanFrame::Payload payload{1, 2, 3, 255, 255, 255, 255, 255};
    const CanFrame::Payload expected{1, 2, 3, 0, 0, 0, 0, 0};
    const CanFrame frame{CanId{0x100}, payload, 3, Timestamp{}};

    EXPECT_EQ(frame.payload(), expected);
}

TEST(CanFrameTest, RejectsNineBytePayloadLength)
{
    EXPECT_THROW((void)(CanFrame{CanId{0x100}, {}, 9, Timestamp{}}),
                 std::length_error);
}

TEST(CanFrameTest, RejectsVeryLargePayloadLength)
{
    EXPECT_THROW((void)(CanFrame{CanId{0x100}, {},
                                std::numeric_limits<std::size_t>::max(), Timestamp{}}),
                 std::length_error);
}

TEST(CanFrameTest, SupportsNonThrowingPayloadSizeValidation)
{
    EXPECT_TRUE(CanFrame::is_valid_payload_size(0));
    EXPECT_TRUE(CanFrame::is_valid_payload_size(8));
    EXPECT_FALSE(CanFrame::is_valid_payload_size(9));
    EXPECT_FALSE(CanFrame::is_valid_payload_size(
        std::numeric_limits<std::size_t>::max()));
}

TEST(CanFrameTest, OwnsItsPayloadInsteadOfAliasingInputStorage)
{
    CanFrame::Payload input{42};
    const CanFrame frame{CanId{0x100}, input, 1, Timestamp{}};
    input[0] = 99;

    EXPECT_EQ(frame.payload()[0], 42);
}

TEST(CanFrameTest, CopiesMovesAndAssignsAsAValue)
{
    const Timestamp timestamp{std::chrono::milliseconds{123}};
    const CanFrame original{CanId{0x100}, {42, 99}, 2, timestamp};
    CanFrame copy{original};
    const CanFrame moved{std::move(copy)};
    CanFrame assigned{CanId{0}, {}, 0, Timestamp{}};
    assigned = original;
    CanFrame move_assigned{CanId{1}, {}, 0, Timestamp{}};
    move_assigned = std::move(assigned);

    for (const CanFrame& frame : {original, moved, move_assigned}) {
        EXPECT_EQ(frame.id(), original.id());
        EXPECT_EQ(frame.payload(), original.payload());
        EXPECT_EQ(frame.payload_size(), original.payload_size());
        EXPECT_EQ(frame.timestamp(), original.timestamp());
    }
}

TEST(CanFrameTest, DoesNotAssumeARealClockEpoch)
{
    const Timestamp timestamp{std::chrono::milliseconds{-5}};
    const CanFrame frame{CanId{0}, {}, 0, timestamp};

    EXPECT_EQ(frame.timestamp(), timestamp);
}

} // namespace
