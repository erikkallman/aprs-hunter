#include "doctest.h"
#include "stations.h"

TEST_CASE("A new station is added")
{
    StationTable table(4);
    table.heard("N0CALL-1", -110.0f, 1000);
    CHECK(table.entries().size() == 1);
}

TEST_CASE("The most recently heard station comes first")
{
    StationTable table(4);
    table.heard("N0CALL-1", -110.0f, 1000);
    table.heard("N0CALL-2", -100.0f, 2000);
    CHECK(table.entries().front().callsign == "N0CALL-2");
}

TEST_CASE("A repeat packet counts towards the same station")
{
    StationTable table(4);
    table.heard("N0CALL-1", -110.0f, 1000);
    table.heard("N0CALL-1", -105.0f, 2000);
    CHECK(table.entries().front().packets == 2u);
}

TEST_CASE("A repeat packet moves the station to the front")
{
    StationTable table(4);
    table.heard("N0CALL-1", -110.0f, 1000);
    table.heard("N0CALL-2", -100.0f, 2000);
    table.heard("N0CALL-1", -105.0f, 3000);
    CHECK(table.entries().front().callsign == "N0CALL-1");
}

TEST_CASE("A repeat packet updates the signal strength")
{
    StationTable table(4);
    table.heard("N0CALL-1", -110.0f, 1000);
    table.heard("N0CALL-1", -95.0f, 2000);
    CHECK(table.entries().front().rssiDbm == doctest::Approx(-95.0f));
}

TEST_CASE("A full table drops the station heard longest ago")
{
    StationTable table(2);
    table.heard("N0CALL-1", -110.0f, 1000);
    table.heard("N0CALL-2", -110.0f, 2000);
    table.heard("N0CALL-3", -110.0f, 3000);
    CHECK(table.entries().back().callsign == "N0CALL-2");
}

TEST_CASE("A full table keeps its size")
{
    StationTable table(2);
    table.heard("N0CALL-1", -110.0f, 1000);
    table.heard("N0CALL-2", -110.0f, 2000);
    table.heard("N0CALL-3", -110.0f, 3000);
    CHECK(table.entries().size() == 2);
}

TEST_CASE("heardWithin is true inside the window")
{
    StationEntry entry{"N0CALL-1", -110.0f, 1000, 1};
    CHECK(heardWithin(entry, 60999, 60000));
}

TEST_CASE("heardWithin is false after the window")
{
    StationEntry entry{"N0CALL-1", -110.0f, 1000, 1};
    CHECK_FALSE(heardWithin(entry, 61000, 60000));
}
