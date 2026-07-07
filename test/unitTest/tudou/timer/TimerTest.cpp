#include <gtest/gtest.h>

#include <chrono>
#include <functional>

#include "tudou/timer/Timer.h"

using namespace std::chrono;

// ───────────────────────── TimerId ─────────────────────────

TEST(TimerIdTest, DefaultConstructIsInvalid) {
    TimerId id;
    EXPECT_FALSE(id.valid());
    EXPECT_EQ(id.value(), 0U);
}

TEST(TimerIdTest, ExplicitConstructIsValid) {
    TimerId id(42);
    EXPECT_TRUE(id.valid());
    EXPECT_EQ(id.value(), 42U);
}

TEST(TimerIdTest, OrderingIsBasedOnValue) {
    TimerId a(1), b(2);
    EXPECT_TRUE(a < b);
    EXPECT_FALSE(b < a);
}

TEST(TimerIdTest, EqualityIsBasedOnValue) {
    TimerId a(7), b(7), c(8);
    EXPECT_TRUE(a == b);
    EXPECT_FALSE(a == c);
}

// ───────────────────────── Timer ─────────────────────────

TEST(TimerTest, OneShotTimerIsNotRepeat) {
    bool called = false;
    Timer timer(TimerId(1), [&]() { called = true; },
                steady_clock::now() + seconds(10), milliseconds(0));

    EXPECT_EQ(timer.get_id().value(), 1U);
    EXPECT_FALSE(timer.is_repeat());
}

TEST(TimerTest, RepeatingTimerIsRepeat) {
    Timer timer(TimerId(2), []() {},
                steady_clock::now() + seconds(1), milliseconds(500));

    EXPECT_TRUE(timer.is_repeat());
}

TEST(TimerTest, RunInvokesCallback) {
    int value = 0;
    Timer timer(TimerId(1), [&]() { value = 42; },
                steady_clock::now(), milliseconds(0));

    timer.run();
    EXPECT_EQ(value, 42);
}

TEST(TimerTest, RunWithNullCallbackDoesNotCrash) {
    // default-constructed std::function is empty
    std::function<void()> empty;
    Timer timer(TimerId(1), std::move(empty), steady_clock::now(), milliseconds(0));

    // should not crash or throw
    timer.run();
}

TEST(TimerTest, RescheduleAdvancesExpirationByInterval) {
    auto t0 = steady_clock::now();
    Timer timer(TimerId(1), []() {}, t0, milliseconds(200));

    auto before = timer.get_expiration();
    timer.reschedule(t0 + seconds(1));
    auto after = timer.get_expiration();

    // reschedule(now) => expiration = now + interval
    auto diff = duration_cast<milliseconds>(after - before);
    EXPECT_GE(diff.count(), 900);  // roughly 1000ms - 200ms initial
    EXPECT_LE(diff.count(), 1200);
}

TEST(TimerTest, ExpirationMatchesConstructionTime) {
    auto t = steady_clock::now() + seconds(5);
    Timer timer(TimerId(1), []() {}, t, milliseconds(0));

    EXPECT_EQ(timer.get_expiration(), t);
}

TEST(TimerTest, IdPersistsAfterReschedule) {
    Timer timer(TimerId(99), []() {},
                steady_clock::now(), milliseconds(100));

    timer.reschedule(steady_clock::now());
    EXPECT_EQ(timer.get_id().value(), 99U);
}