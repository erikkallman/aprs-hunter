/**
 * Screen logic for the OLED, kept free of hardware so it can be tested on a
 * computer. oled_view draws what this module decides.
 */

#ifndef DISPLAY_LOGIC_H
#define DISPLAY_LOGIC_H

#include <cstdint>
#include <string>

#include "records.h"

/**
 * Animated: the sonar runs between packets.
 * NotifyOnly: the screen stays off between packets, to save battery.
 * The numeric values are stored in flash; do not renumber them.
 */
enum class DisplayMode : uint8_t
{
    Animated = 0,
    NotifyOnly = 1,
};

enum class Screen : uint8_t
{
    Off,
    Splash,
    Notification,
    Sonar,
};

const char *displayModeName(DisplayMode mode); // "animated" or "notify"
DisplayMode toggledMode(DisplayMode mode);

/** Mode from a stored byte; unknown values give Animated. */
DisplayMode displayModeFromByte(uint8_t value);

/**
 * Parse a serial command: "display toggle", "display animated", or
 * "display notify", in any letter case.
 *
 * @param line the text received over serial
 * @param current the mode now, needed for "toggle"
 * @param result set to the new mode when the line is a display command
 * @return true if the line is a display command
 */
bool parseDisplayCommand(const std::string &line, DisplayMode current, DisplayMode &result);

/**
 * What the screen shows now. A notification wins over the splash, and the
 * splash wins over the idle screen.
 */
Screen chooseScreen(DisplayMode mode, bool splashActive, bool notificationActive);

/**
 * A timer for one window of time, safe across the 32-bit millisecond wrap.
 */
class OneShotTimer
{
  public:
    void start(uint32_t nowMs, uint32_t durationMs);
    bool active(uint32_t nowMs) const;
    uint32_t elapsed(uint32_t nowMs) const;

  private:
    bool started = false;
    uint32_t startMs = 0;
    uint32_t durationMs = 0;
};

struct Point
{
    int x;
    int y;
};

/** Radius of the sonar ping ring: grows from 0 to maxRadius once per period. */
int pingRadius(uint32_t nowMs, uint32_t periodMs, int maxRadius);

/** Fixed angle for a callsign, 0 to 359 degrees (djb2 hash modulo 360). */
int callsignAngleDegrees(const std::string &callsign);

/**
 * Distance of a station's dot from the centre: -70 dBm or stronger sits on
 * the inner radius, -130 dBm or weaker on the outer radius.
 */
int signalRadius(float rssiDbm, int innerRadius, int outerRadius);

/** Point at a compass angle: 0 is straight up, 90 is right. */
Point polarPoint(int centreX, int centreY, int radius, int angleDegrees);

/**
 * What the notification shows about a packet.
 */
struct PacketSummary
{
    uint32_t seq = 0;
    std::string callsign; // "?" when the frame is not valid LoRa APRS
    bool frameValid = false;
    float rssiDbm = 0.0f;
    float snrDb = 0.0f;
    bool crcOk = false;
};

PacketSummary summarizePacket(const RxRecord &record);

#endif
