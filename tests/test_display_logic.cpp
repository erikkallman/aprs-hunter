#include "doctest.h"
#include "display_logic.h"
#include "test_helpers.h"

// ---- Screen choice ----

TEST_CASE("A notification wins over the splash")
{
    CHECK(chooseScreen(DisplayMode::Animated, true, true) == Screen::Notification);
}

TEST_CASE("The splash wins over the idle screen")
{
    CHECK(chooseScreen(DisplayMode::Animated, true, false) == Screen::Splash);
}

TEST_CASE("Animated mode shows the sonar when idle")
{
    CHECK(chooseScreen(DisplayMode::Animated, false, false) == Screen::Sonar);
}

TEST_CASE("Notify-only mode turns the screen off when idle")
{
    CHECK(chooseScreen(DisplayMode::NotifyOnly, false, false) == Screen::Off);
}

TEST_CASE("Notify-only mode still shows notifications")
{
    CHECK(chooseScreen(DisplayMode::NotifyOnly, false, true) == Screen::Notification);
}

// ---- Display mode ----

TEST_CASE("displayModeName for notify-only mode")
{
    CHECK(std::string(displayModeName(DisplayMode::NotifyOnly)) == "notify");
}

TEST_CASE("toggledMode switches animated to notify-only")
{
    CHECK(toggledMode(DisplayMode::Animated) == DisplayMode::NotifyOnly);
}

TEST_CASE("A stored 1 means notify-only mode")
{
    CHECK(displayModeFromByte(1) == DisplayMode::NotifyOnly);
}

TEST_CASE("An unknown stored value means animated mode")
{
    CHECK(displayModeFromByte(7) == DisplayMode::Animated);
}

// ---- Serial commands ----

namespace
{
    bool parse(const std::string &line, DisplayMode current, DisplayMode &result)
    {
        return parseDisplayCommand(line, current, result);
    }
}

TEST_CASE("'display toggle' switches the current mode")
{
    DisplayMode result = DisplayMode::Animated;
    parse("display toggle", DisplayMode::Animated, result);
    CHECK(result == DisplayMode::NotifyOnly);
}

TEST_CASE("Commands ignore letter case")
{
    DisplayMode result = DisplayMode::Animated;
    parse("DISPLAY Notify", DisplayMode::Animated, result);
    CHECK(result == DisplayMode::NotifyOnly);
}

TEST_CASE("Commands ignore surrounding spaces and line endings")
{
    DisplayMode result = DisplayMode::NotifyOnly;
    parse("  display animated \r", DisplayMode::NotifyOnly, result);
    CHECK(result == DisplayMode::Animated);
}

TEST_CASE("'display' alone is not a command")
{
    DisplayMode result = DisplayMode::Animated;
    CHECK_FALSE(parse("display", DisplayMode::Animated, result));
}

TEST_CASE("An unknown display option is not a command")
{
    DisplayMode result = DisplayMode::Animated;
    CHECK_FALSE(parse("display blink", DisplayMode::Animated, result));
}

TEST_CASE("Other text is not a command")
{
    DisplayMode result = DisplayMode::Animated;
    CHECK_FALSE(parse("hello", DisplayMode::Animated, result));
}

// ---- One-shot timer ----

TEST_CASE("A timer that never started is not active")
{
    OneShotTimer timer;
    CHECK_FALSE(timer.active(1000));
}

TEST_CASE("A timer is active inside its window")
{
    OneShotTimer timer;
    timer.start(1000, 4000);
    CHECK(timer.active(4999));
}

TEST_CASE("A timer is not active at the end of its window")
{
    OneShotTimer timer;
    timer.start(1000, 4000);
    CHECK_FALSE(timer.active(5000));
}

TEST_CASE("A timer stays active across the millisecond counter wrap")
{
    OneShotTimer timer;
    timer.start(0xFFFFF000u, 0x2000u);
    CHECK(timer.active(0x00000500u));
}

TEST_CASE("A timer reports the time since it started")
{
    OneShotTimer timer;
    timer.start(1000, 4000);
    CHECK(timer.elapsed(1300) == 300u);
}

// ---- Sonar geometry ----

TEST_CASE("The ping ring starts at radius 0")
{
    CHECK(pingRadius(0, 2500, 34) == 0);
}

TEST_CASE("The ping ring is half grown half way through the period")
{
    CHECK(pingRadius(1250, 2500, 34) == 17);
}

TEST_CASE("The ping ring starts again after each period")
{
    CHECK(pingRadius(3750, 2500, 34) == 17);
}

TEST_CASE("A callsign always gets the same angle")
{
    // djb2("N0CALL") = 3244022271; 3244022271 % 360 = 351
    CHECK(callsignAngleDegrees("N0CALL") == 351);
}

TEST_CASE("Another callsign gets another angle")
{
    // djb2("N0CALL-7") = 2277137347; 2277137347 % 360 = 187
    CHECK(callsignAngleDegrees("N0CALL-7") == 187);
}

TEST_CASE("A strong signal sits on the inner radius")
{
    CHECK(signalRadius(-70.0f, 6, 30) == 6);
}

TEST_CASE("A weak signal sits on the outer radius")
{
    CHECK(signalRadius(-130.0f, 6, 30) == 30);
}

TEST_CASE("A middle signal sits half way")
{
    CHECK(signalRadius(-100.0f, 6, 30) == 18);
}

TEST_CASE("Signals beyond the scale are clamped")
{
    CHECK(signalRadius(-40.0f, 6, 30) == 6);
}

TEST_CASE("Angle 0 points straight up")
{
    Point p = polarPoint(64, 32, 10, 0);
    CHECK((p.x == 64 && p.y == 22));
}

TEST_CASE("Angle 90 points right")
{
    Point p = polarPoint(64, 32, 10, 90);
    CHECK((p.x == 74 && p.y == 32));
}

// ---- Packet summary ----

namespace
{
    RxRecord packet(const std::vector<uint8_t> &data)
    {
        RxRecord record;
        record.seq = 12;
        record.rssiDbm = -112.5f;
        record.snrDb = -7.25f;
        record.crcOk = true;
        record.data = data;
        return record;
    }
}

TEST_CASE("The summary takes the callsign from the frame")
{
    CHECK(summarizePacket(packet(loraAprsFrame("N0CALL-7>APRS:>hi"))).callsign == "N0CALL-7");
}

TEST_CASE("The summary marks a valid frame")
{
    CHECK(summarizePacket(packet(loraAprsFrame("N0CALL-7>APRS:>hi"))).frameValid);
}

TEST_CASE("The summary shows '?' for a frame that is not LoRa APRS")
{
    CHECK(summarizePacket(packet({0x01, 0x02})).callsign == "?");
}

TEST_CASE("The summary keeps the packet number")
{
    CHECK(summarizePacket(packet({0x01})).seq == 12u);
}
