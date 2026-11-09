#include "doctest.h"
#include "double_reset.h"

namespace
{
    // Stands in for the flash storage that survives a reset.
    class FakeFlagStore : public FlagStore
    {
      public:
        bool value = false;
        bool read() override { return value; }
        void write(bool newValue) override { value = newValue; }
    };
}

TEST_CASE("The first boot is not a double reset")
{
    FakeFlagStore store;
    DoubleResetDetector detector(store, 3000);
    CHECK_FALSE(detector.detect());
}

TEST_CASE("A boot inside the window of the previous boot is a double reset")
{
    FakeFlagStore store;
    DoubleResetDetector first(store, 3000);
    first.detect();
    first.update(1000); // still inside the window when the reset comes

    DoubleResetDetector second(store, 3000);
    CHECK(second.detect());
}

TEST_CASE("A boot after the window is not a double reset")
{
    FakeFlagStore store;
    DoubleResetDetector first(store, 3000);
    first.detect();
    first.update(3000);

    DoubleResetDetector second(store, 3000);
    CHECK_FALSE(second.detect());
}

TEST_CASE("A detection clears the flag, so a third quick reset does not count")
{
    FakeFlagStore store;
    DoubleResetDetector first(store, 3000);
    first.detect();
    DoubleResetDetector second(store, 3000);
    second.detect();

    DoubleResetDetector third(store, 3000);
    CHECK_FALSE(third.detect());
}

TEST_CASE("The detector writes the flag only once after the window")
{
    FakeFlagStore store;
    DoubleResetDetector detector(store, 3000);
    detector.detect();
    detector.update(3000);
    store.value = true; // anything written after this point would be a bug
    detector.update(4000);
    CHECK(store.value);
}
