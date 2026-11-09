#include "doctest.h"
#include "aprs_frame.h"
#include "test_helpers.h"

namespace
{
    AprsFrame parse(const std::string &text, bool withPrefix = true)
    {
        std::vector<uint8_t> bytes = loraAprsFrame(text, withPrefix);
        return parseLoRaAprsFrame(bytes.data(), bytes.size());
    }

    const char *TRACKER = "N0CALL-7>APLRT1,WIDE1-1,qAR:!5920.00N/01804.00E>Test tracker";
}

TEST_CASE("A frame with a path is valid")
{
    CHECK(parse(TRACKER).valid);
}

TEST_CASE("The source is the text before the '>'")
{
    CHECK(parse(TRACKER).source == "N0CALL-7");
}

TEST_CASE("The destination is the first field after the '>'")
{
    CHECK(parse(TRACKER).destination == "APLRT1");
}

TEST_CASE("The path is the rest of the header, comma-separated")
{
    CHECK(parse(TRACKER).path == "WIDE1-1,qAR");
}

TEST_CASE("The info is everything after the first ':'")
{
    CHECK(parse(TRACKER).info == "!5920.00N/01804.00E>Test tracker");
}

TEST_CASE("A frame without a path has an empty path")
{
    CHECK(parse("N0CALL>APRS:>status text").path.empty());
}

TEST_CASE("The info keeps every ':' after the first one")
{
    // An APRS message: the addressee field is wrapped in ':'.
    CHECK(parse("N0CALL>APRS::N0CALL-9 :hello{1").info == ":N0CALL-9 :hello{1");
}

TEST_CASE("A frame without the LoRa APRS prefix is invalid")
{
    AprsFrame frame = parse(TRACKER, false);
    CHECK(frame.error == "no LoRa APRS prefix");
}

TEST_CASE("A frame without the prefix reports that it has none")
{
    CHECK_FALSE(parse(TRACKER, false).hasPrefix);
}

TEST_CASE("A frame shorter than the prefix is invalid")
{
    const uint8_t bytes[] = {0x3C, 0xFF};
    CHECK(parseLoRaAprsFrame(bytes, sizeof(bytes)).error == "shorter than the LoRa APRS prefix");
}

TEST_CASE("A frame without '>' is invalid")
{
    CHECK(parse("N0CALL:hello").error == "no '>' in the header");
}

TEST_CASE("A frame without ':' is invalid")
{
    CHECK(parse("N0CALL>APRS").error == "no ':' after the header");
}

TEST_CASE("A frame with an empty source is invalid")
{
    CHECK(parse(">APRS:hello").error == "empty source");
}

TEST_CASE("A '>' inside the info does not count as the header separator")
{
    CHECK(parse("N0CALL:>status").error == "no '>' in the header");
}

TEST_CASE("Bytes after a zero byte stay in the info")
{
    std::vector<uint8_t> bytes = loraAprsFrame("N0CALL>APRS:ab");
    bytes.push_back(0x00);
    bytes.push_back('c');
    CHECK(parseLoRaAprsFrame(bytes.data(), bytes.size()).info == std::string("ab\0c", 4));
}
