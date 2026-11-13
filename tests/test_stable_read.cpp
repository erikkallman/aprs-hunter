#include <vector>

#include "doctest.h"
#include "stable_read.h"

namespace
{
    // Returns the given values in order, then repeats the last one.
    class ScriptedSource : public RssiSource
    {
      public:
        explicit ScriptedSource(std::vector<float> script) : values(script) {}

        float read() override
        {
            float value = values[reads < values.size() ? reads : values.size() - 1];
            reads++;
            return value;
        }

        size_t reads = 0;

      private:
        std::vector<float> values;
    };
}

TEST_CASE("Two reads that agree give the value")
{
    ScriptedSource source({-101.0f, -101.0f});
    CHECK(readStable(source, 4).value == -101.0f);
}

TEST_CASE("Two reads that agree are ok")
{
    ScriptedSource source({-101.0f, -101.0f});
    CHECK(readStable(source, 4).ok);
}

TEST_CASE("Reading stops once two reads agree")
{
    ScriptedSource source({-101.0f, -101.0f, -90.0f});
    readStable(source, 4);
    CHECK(source.reads == 2);
}

TEST_CASE("Two reads that agree have no mismatches")
{
    ScriptedSource source({-101.0f, -101.0f});
    CHECK(readStable(source, 4).mismatches == 0);
}

TEST_CASE("A torn first read is dropped")
{
    ScriptedSource source({-164.0f, -101.0f, -101.0f});
    CHECK(readStable(source, 4).value == -101.0f);
}

TEST_CASE("A torn first read counts one mismatch")
{
    ScriptedSource source({-164.0f, -101.0f, -101.0f});
    CHECK(readStable(source, 4).mismatches == 1);
}

TEST_CASE("A torn read between two good reads is dropped")
{
    ScriptedSource source({-101.0f, -37.0f, -101.0f, -101.0f});
    CHECK(readStable(source, 4).value == -101.0f);
}

TEST_CASE("Reads that never agree are not ok")
{
    ScriptedSource source({-101.0f, -100.0f, -101.0f, -100.0f});
    CHECK_FALSE(readStable(source, 4).ok);
}

TEST_CASE("Reads that never agree stop at the read limit")
{
    ScriptedSource source({-101.0f, -100.0f, -101.0f, -100.0f, -100.0f});
    readStable(source, 4);
    CHECK(source.reads == 4);
}

TEST_CASE("Reads that never agree count every mismatch")
{
    ScriptedSource source({-101.0f, -100.0f, -101.0f, -100.0f});
    CHECK(readStable(source, 4).mismatches == 3);
}

TEST_CASE("A read limit below two still reads twice")
{
    ScriptedSource source({-101.0f, -101.0f});
    CHECK(readStable(source, 1).ok);
}

namespace
{
    const ConfirmRule RULE = {6.0f, 2.0f};

    StableRead agreedOn(float value)
    {
        StableRead reading;
        reading.ok = true;
        reading.value = value;
        return reading;
    }
}

TEST_CASE("A value near the reference needs no confirm read")
{
    ScriptedSource shifted({-50.0f});
    confirmFarRead(agreedOn(-103.0f), shifted, -101.0f, RULE);
    CHECK(shifted.reads == 0);
}

TEST_CASE("A value near the reference stays ok")
{
    ScriptedSource shifted({-50.0f});
    CHECK(confirmFarRead(agreedOn(-103.0f), shifted, -101.0f, RULE).ok);
}

TEST_CASE("A reading whose reads never agreed needs no confirm read")
{
    ScriptedSource shifted({-101.0f});
    confirmFarRead(StableRead(), shifted, -101.0f, RULE);
    CHECK(shifted.reads == 0);
}

TEST_CASE("A far value takes one confirm read")
{
    ScriptedSource shifted({-93.0f});
    confirmFarRead(agreedOn(-93.0f), shifted, -101.0f, RULE);
    CHECK(shifted.reads == 1);
}

TEST_CASE("A far value that the confirm read repeats is kept")
{
    ScriptedSource shifted({-93.0f});
    CHECK(confirmFarRead(agreedOn(-93.0f), shifted, -101.0f, RULE).value == -93.0f);
}

TEST_CASE("A far value with a confirm read inside the tolerance is kept")
{
    ScriptedSource shifted({-91.0f});
    CHECK(confirmFarRead(agreedOn(-93.0f), shifted, -101.0f, RULE).ok);
}

TEST_CASE("A kept far value is not marked unconfirmed")
{
    ScriptedSource shifted({-93.0f});
    CHECK_FALSE(confirmFarRead(agreedOn(-93.0f), shifted, -101.0f, RULE).unconfirmed);
}

TEST_CASE("A far high value that the confirm read does not repeat is not ok")
{
    ScriptedSource shifted({-101.0f});
    CHECK_FALSE(confirmFarRead(agreedOn(-37.0f), shifted, -101.0f, RULE).ok);
}

TEST_CASE("A far low value that the confirm read does not repeat is not ok")
{
    ScriptedSource shifted({-103.0f});
    CHECK_FALSE(confirmFarRead(agreedOn(-108.0f), shifted, -101.0f, RULE).ok);
}

TEST_CASE("A far value that the confirm read does not repeat is marked unconfirmed")
{
    ScriptedSource shifted({-101.0f});
    CHECK(confirmFarRead(agreedOn(-37.0f), shifted, -101.0f, RULE).unconfirmed);
}

TEST_CASE("A far value that the confirm read does not repeat has no value")
{
    ScriptedSource shifted({-101.0f});
    CHECK(confirmFarRead(agreedOn(-37.0f), shifted, -101.0f, RULE).value == 0.0f);
}

TEST_CASE("A confirm read that disagrees counts one more mismatch")
{
    ScriptedSource shifted({-101.0f});
    StableRead reading = agreedOn(-37.0f);
    reading.mismatches = 2;
    CHECK(confirmFarRead(reading, shifted, -101.0f, RULE).mismatches == 3);
}

TEST_CASE("A value exactly the far distance away takes a confirm read")
{
    ScriptedSource shifted({-95.0f});
    confirmFarRead(agreedOn(-95.0f), shifted, -101.0f, RULE);
    CHECK(shifted.reads == 1);
}
