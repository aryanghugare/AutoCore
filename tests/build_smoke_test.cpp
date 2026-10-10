#include <gtest/gtest.h>

#include <optional>

TEST(BuildSmokeTest, SupportsCpp17)
{
    const std::optional<int> value{17};

    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(value.value(), 17);
}
