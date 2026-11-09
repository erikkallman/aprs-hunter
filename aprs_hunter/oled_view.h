/**
 * Drawing on the T3's 128x64 SSD1306 OLED. This file only draws; the
 * decisions (what to show, where dots go) live in display_logic.
 */

#ifndef OLED_VIEW_H
#define OLED_VIEW_H

#include <Adafruit_SSD1306.h>
#include <Wire.h>
#include <string>

#include "display_logic.h"
#include "stations.h"

class OledView
{
  public:
    /**
     * Start I2C and the screen.
     *
     * @param contrast 0 to 255; lower is dimmer and uses less power
     * @param rotation 0 for normal, 2 for upside down
     * @return false if the screen does not answer
     */
    bool begin(uint8_t sdaPin, uint8_t sclPin, uint8_t contrast, uint8_t rotation);

    /** Turn the screen on or off. Off draws almost no current. */
    void setPower(bool on);

    void drawSplash(DisplayMode mode);
    void drawNotification(const PacketSummary &packet, uint32_t elapsedMs);
    void drawSonar(const StationTable &stations, uint32_t nowMs, uint32_t packetCount);

  private:
    Adafruit_SSD1306 display{128, 64, &Wire, -1};
    bool ready = false;
    bool powered = false;

    void drawCentered(const std::string &text, int y, uint8_t size);
    void drawRight(const std::string &text, int y);
};

#endif
