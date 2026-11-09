#include "oled_view.h"

#include <cstdio>
#include <cstdlib>

namespace
{
    const int SCREEN_WIDTH = 128;
    const int SCREEN_HEIGHT = 64;
    const int CENTRE_X = 64;
    const int CENTRE_Y = 32;
    const int CHAR_WIDTH = 6; // default 5x7 font plus one pixel of spacing
    const int CHAR_HEIGHT = 8;

    // Sonar
    const int SONAR_INNER_RADIUS = 6;
    const int SONAR_OUTER_RADIUS = 30;
    const uint32_t PING_PERIOD_MS = 2600;
    const int PING_MAX_RADIUS = 36;
    const int ECHO_TOLERANCE = 2;                     // ring within 2 px of a dot lights it up
    const uint32_t STATION_WINDOW_MS = 15UL * 60 * 1000; // dots stay for 15 minutes
    const uint32_t FRESH_MS = 60UL * 1000;            // bigger dot for the first minute

    // Notification
    const uint32_t BURST_MS = 400;
    const int BURST_MAX_RADIUS = 72;
    const size_t CALLSIGN_MAX_CHARS = 10; // 10 characters at text size 2 fill 120 px
}

bool OledView::begin(uint8_t sdaPin, uint8_t sclPin, uint8_t contrast, uint8_t rotation)
{
    Wire.begin(sdaPin, sclPin);
    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C, true, false)) {
        return false;
    }
    display.ssd1306_command(SSD1306_SETCONTRAST);
    display.ssd1306_command(contrast);
    display.setRotation(rotation);
    display.setTextColor(SSD1306_WHITE);
    display.setTextWrap(false);
    display.clearDisplay();
    display.display();
    ready = true;
    powered = true;
    return true;
}

void OledView::setPower(bool on)
{
    if (!ready || on == powered) {
        return;
    }
    display.ssd1306_command(on ? SSD1306_DISPLAYON : SSD1306_DISPLAYOFF);
    powered = on;
}

void OledView::drawSplash(DisplayMode mode)
{
    if (!ready) {
        return;
    }
    display.clearDisplay();
    drawCentered("APRS HUNTER", 12, 1);
    drawCentered(mode == DisplayMode::Animated ? "sonar on" : "notify only", 30, 1);
    drawCentered("reset x2: switch", 48, 1);
    display.display();
}

void OledView::drawNotification(const PacketSummary &packet, uint32_t elapsedMs)
{
    if (!ready) {
        return;
    }
    display.clearDisplay();

    // A short burst of two rings, then the text.
    if (elapsedMs < BURST_MS) {
        int radius = static_cast<int>(elapsedMs * BURST_MAX_RADIUS / BURST_MS);
        display.drawCircle(CENTRE_X, CENTRE_Y, radius, SSD1306_WHITE);
        if (radius > 8) {
            display.drawCircle(CENTRE_X, CENTRE_Y, radius - 8, SSD1306_WHITE);
        }
        display.display();
        return;
    }

    display.setTextSize(1);
    display.setCursor(0, 0);
    display.print(("#" + std::to_string(packet.seq)).c_str());
    if (!packet.crcOk) {
        drawRight("CRC FAIL", 0);
    }

    drawCentered(packet.callsign.substr(0, CALLSIGN_MAX_CHARS), 22, 2);

    char signal[32];
    std::snprintf(signal, sizeof(signal), "%.0f dBm  %.1f dB", packet.rssiDbm, packet.snrDb);
    drawCentered(signal, 48, 1);
    display.display();
}

void OledView::drawSonar(const StationTable &stations, uint32_t nowMs, uint32_t packetCount)
{
    if (!ready) {
        return;
    }
    display.clearDisplay();

    display.fillCircle(CENTRE_X, CENTRE_Y, 1, SSD1306_WHITE);

    int ring = pingRadius(nowMs, PING_PERIOD_MS, PING_MAX_RADIUS);
    if (ring > 2) {
        display.drawCircle(CENTRE_X, CENTRE_Y, ring, SSD1306_WHITE);
    }

    for (const StationEntry &station : stations.entries()) {
        if (!heardWithin(station, nowMs, STATION_WINDOW_MS)) {
            continue;
        }
        int distance = signalRadius(station.rssiDbm, SONAR_INNER_RADIUS, SONAR_OUTER_RADIUS);
        Point dot = polarPoint(CENTRE_X, CENTRE_Y, distance, callsignAngleDegrees(station.callsign));

        if (std::abs(ring - distance) <= ECHO_TOLERANCE) {
            display.fillCircle(dot.x, dot.y, 2, SSD1306_WHITE); // echo as the ring passes
        } else if (heardWithin(station, nowMs, FRESH_MS)) {
            display.fillCircle(dot.x, dot.y, 1, SSD1306_WHITE);
        } else {
            display.drawPixel(dot.x, dot.y, SSD1306_WHITE);
        }
    }

    if (packetCount > 0) {
        display.setTextSize(1);
        drawRight(std::to_string(packetCount), SCREEN_HEIGHT - CHAR_HEIGHT);
    }
    display.display();
}

void OledView::drawCentered(const std::string &text, int y, uint8_t size)
{
    int width = static_cast<int>(text.size()) * CHAR_WIDTH * size;
    int x = (SCREEN_WIDTH - width) / 2;
    display.setTextSize(size);
    display.setCursor(x < 0 ? 0 : x, y);
    display.print(text.c_str());
}

void OledView::drawRight(const std::string &text, int y)
{
    display.setCursor(SCREEN_WIDTH - static_cast<int>(text.size()) * CHAR_WIDTH, y);
    display.print(text.c_str());
}
