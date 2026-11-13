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
