#include "display_logic.h"

#include <cctype>
#include <cmath>
#include <sstream>
#include <vector>

#include "aprs_frame.h"

const char *displayModeName(DisplayMode mode)
{
    return mode == DisplayMode::NotifyOnly ? "notify" : "animated";
}

DisplayMode toggledMode(DisplayMode mode)
{
    return mode == DisplayMode::Animated ? DisplayMode::NotifyOnly : DisplayMode::Animated;
}

DisplayMode displayModeFromByte(uint8_t value)
{
    return value == static_cast<uint8_t>(DisplayMode::NotifyOnly) ? DisplayMode::NotifyOnly
                                                                  : DisplayMode::Animated;
}

bool parseDisplayCommand(const std::string &line, DisplayMode current, DisplayMode &result)
{
    std::string lower;
    for (unsigned char c : line) {
        lower += static_cast<char>(std::tolower(c));
    }

    std::istringstream words(lower);
    std::vector<std::string> parts;
    std::string word;
    while (words >> word) {
        parts.push_back(word);
    }
    if (parts.size() != 2 || parts[0] != "display") {
        return false;
    }

    if (parts[1] == "toggle") {
        result = toggledMode(current);
    } else if (parts[1] == "animated") {
        result = DisplayMode::Animated;
    } else if (parts[1] == "notify") {
        result = DisplayMode::NotifyOnly;
    } else {
        return false;
    }
    return true;
}

Screen chooseScreen(DisplayMode mode, bool splashActive, bool notificationActive)
{
    if (notificationActive) {
        return Screen::Notification;
    }
    if (splashActive) {
        return Screen::Splash;
    }
    return mode == DisplayMode::NotifyOnly ? Screen::Off : Screen::Sonar;
}

void OneShotTimer::start(uint32_t nowMs, uint32_t duration)
{
    started = true;
    startMs = nowMs;
    durationMs = duration;
}

bool OneShotTimer::active(uint32_t nowMs) const
{
    return started && elapsed(nowMs) < durationMs;
}

uint32_t OneShotTimer::elapsed(uint32_t nowMs) const
{
    return nowMs - startMs; // unsigned subtraction handles the wrap
}

int pingRadius(uint32_t nowMs, uint32_t periodMs, int maxRadius)
{
    return static_cast<int>((nowMs % periodMs) * static_cast<uint32_t>(maxRadius) / periodMs);
}

int callsignAngleDegrees(const std::string &callsign)
{
    uint32_t hash = 5381;
    for (unsigned char c : callsign) {
        hash = (hash << 5) + hash + c; // djb2: hash * 33 + c
    }
    return static_cast<int>(hash % 360);
}

int signalRadius(float rssiDbm, int innerRadius, int outerRadius)
{
    const float weakest = -130.0f;
    const float strongest = -70.0f;
    float strength = (rssiDbm - weakest) / (strongest - weakest);
    if (strength < 0.0f) {
        strength = 0.0f;
    }
    if (strength > 1.0f) {
        strength = 1.0f;
    }
    return outerRadius - static_cast<int>(std::lround(strength * (outerRadius - innerRadius)));
}

Point polarPoint(int centreX, int centreY, int radius, int angleDegrees)
{
    const double radians = angleDegrees * 3.14159265358979323846 / 180.0;
    Point p;
    p.x = centreX + static_cast<int>(std::lround(radius * std::sin(radians)));
    p.y = centreY - static_cast<int>(std::lround(radius * std::cos(radians)));
    return p;
}

PacketSummary summarizePacket(const RxRecord &record)
{
    PacketSummary summary;
    summary.seq = record.seq;
    summary.rssiDbm = record.rssiDbm;
    summary.snrDb = record.snrDb;
    summary.crcOk = record.crcOk;

    AprsFrame frame = parseLoRaAprsFrame(record.data.data(), record.data.size());
    summary.frameValid = frame.valid;
    summary.callsign = frame.valid ? frame.source : "?";
    return summary;
}
