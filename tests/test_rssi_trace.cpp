#include "doctest.h"
#include "rssi_trace.h"

namespace
{
    TraceBuffer readsAt(std::initializer_list<float> values)
    {
        TraceBuffer buffer;
        uint32_t us = 1000;
        for (float value : values) {
            buffer.add(value, us);
            us += 25;
        }
        return buffer;
    }
}

TEST_CASE("A new trace buffer is empty")
{
    TraceBuffer buffer;
    CHECK(buffer.size() == 0);
}

TEST_CASE("The trace buffer keeps reads in order")
{
    TraceBuffer buffer = readsAt({-101.0f, -69.0f});
    CHECK(buffer.at(1).dbm == -69.0f);
}

TEST_CASE("The trace buffer keeps the time of each read")
{
    TraceBuffer buffer = readsAt({-101.0f, -69.0f});
    CHECK(buffer.at(1).us == 1025);
}

TEST_CASE("The trace buffer ignores reads past its capacity")
{
    TraceBuffer buffer;
    for (size_t i = 0; i < TRACE_CAPACITY + 3; i++) {
        buffer.add(-101.0f, 0);
    }
    CHECK(buffer.size() == TRACE_CAPACITY);
}

TEST_CASE("Clearing the trace buffer empties it")
{
    TraceBuffer buffer = readsAt({-101.0f});
    buffer.clear();
    CHECK(buffer.size() == 0);
}

TEST_CASE("Reads near the reference are no jump")
{
    CHECK_FALSE(hasJump(readsAt({-101.0f, -100.0f, -104.0f}), -101.3f, 6.0f));
}

TEST_CASE("A read far below the reference is a jump")
{
    CHECK(hasJump(readsAt({-101.0f, -116.0f, -101.0f}), -101.3f, 6.0f));
}

TEST_CASE("A read far above the reference is a jump")
{
    CHECK(hasJump(readsAt({-101.0f, -69.0f}), -101.3f, 6.0f));
}

TEST_CASE("A read exactly the jump distance away is a jump")
{
    CHECK(hasJump(readsAt({-107.0f}), -101.0f, 6.0f));
}

TEST_CASE("An empty trace has no jump")
{
    CHECK_FALSE(hasJump(TraceBuffer(), -101.0f, 6.0f));
}

TEST_CASE("The gate allows the first trace")
{
    TraceGate gate(1000, 5);
    CHECK(gate.allow(50));
}

TEST_CASE("The gate refuses a trace inside the interval")
{
    TraceGate gate(1000, 5);
    gate.allow(50);
    CHECK_FALSE(gate.allow(900));
}

TEST_CASE("The gate allows a trace once the interval has passed")
{
    TraceGate gate(1000, 5);
    gate.allow(50);
    CHECK(gate.allow(1050));
}

TEST_CASE("The gate reports the traces it refused before the one it allows")
{
    TraceGate gate(1000, 5);
    gate.allow(50);
    gate.allow(300);
    gate.allow(600);
    gate.allow(1100);
    CHECK(gate.missedBefore() == 2);
}

TEST_CASE("The first trace has no refused traces before it")
{
    TraceGate gate(1000, 5);
    gate.allow(50);
    CHECK(gate.missedBefore() == 0);
}

TEST_CASE("The gate stops at its trace limit")
{
    TraceGate gate(1000, 2);
    gate.allow(0);
    gate.allow(1000);
    CHECK_FALSE(gate.allow(2000));
}

TEST_CASE("The gate works across the millisecond counter wrapping")
{
    TraceGate gate(1000, 5);
    gate.allow(0xFFFFFF00u);
    CHECK(gate.allow(0x00000400u));
}

TEST_CASE("A gate with a limit of 0 allows nothing")
{
    TraceGate gate(1000, 0);
    CHECK_FALSE(gate.allow(50));
}
