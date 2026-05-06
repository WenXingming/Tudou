#include <gtest/gtest.h>

#include <type_traits>

#include "base/NonCopyable.h"

namespace {

class SampleNonCopyable : protected NonCopyable {
public:
    SampleNonCopyable() = default;
    ~SampleNonCopyable() = default;
};

} // namespace

TEST(NonCopyableTest, DerivedTypeRejectsCopyAndMoveSemantics) {
    EXPECT_FALSE(std::is_copy_constructible<SampleNonCopyable>::value);
    EXPECT_FALSE(std::is_copy_assignable<SampleNonCopyable>::value);
    EXPECT_FALSE(std::is_move_constructible<SampleNonCopyable>::value);
    EXPECT_FALSE(std::is_move_assignable<SampleNonCopyable>::value);
}