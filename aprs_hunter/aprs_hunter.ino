/**
 * APRS hunter: a receive-only LoRa APRS logger for the LilyGO T3 V1.6.1
 * (433 MHz version, SX1278 radio).
 *
 * The sketch prints one JSON object per line over USB serial at 115200 baud:
 *   boot    - firmware version, display mode, and radio settings, at start-up
 *   rx      - every received packet, with signal metadata and the raw bytes
 *   noise   - a noise floor summary every 10 seconds
 *   display - a display mode change
 *   error   - radio, screen, or command errors
 *
 * The OLED shows a short notification for every packet. Between packets it
 * shows a sonar animation (animated mode) or stays off (notify-only mode).
 * Switch modes by pressing reset twice within 3 seconds, or by sending
 * "display toggle", "display animated", or "display notify" over serial.
 * The board remembers the mode.
 *
 * This sketch never transmits. The board LED flashes on every packet.
 */

#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>
#include <Preferences.h>
#include <Wire.h>
#include <esp_system.h>

#include "display_logic.h"
#include "double_reset.h"
#include "oled_view.h"
#include "records.h"
#include "stations.h"

#define FIRMWARE_VERSION "0.2.0"

// LilyGO T3 V1.6.1 radio pins (same on the 433 MHz and 868 MHz versions)
#define LORA_SCK 5
#define LORA_MISO 19
#define LORA_MOSI 27
#define LORA_SS 18
#define LORA_RST 23
#define LORA_DIO0 26
#define LORA_DIO1 33

// LilyGO T3 V1.6.1 OLED (SSD1306, I2C address 0x3C)
#define OLED_SDA 21
#define OLED_SCL 22

#define BOARD_LED 25
#define SERIAL_BAUD 115200

// LoRa APRS in Europe. These match the defaults of CA2RXU's LoRa_APRS_iGate.
#define APRS_FREQUENCY_HZ 433775000UL
#define APRS_BANDWIDTH_HZ 125000UL
#define APRS_SPREADING_FACTOR 12
#define APRS_CODING_RATE 5      // 4/5
#define APRS_SYNC_WORD 0x12     // RadioLib default, used by LoRa APRS
#define APRS_PREAMBLE_LENGTH 8

#define NOISE_SAMPLE_INTERVAL_MS 100
#define NOISE_REPORT_INTERVAL_MS 10000
#define LED_FLASH_MS 100

// Display settings
#define DISPLAY_CONTRAST 0x40      // 0-255. Lower is dimmer and uses less power.
#define DISPLAY_ROTATION 0         // 2 turns the picture upside down.
#define DISPLAY_FRAME_MS 80        // about 12 frames per second
#define SPLASH_MS 2500
#define NOTIFICATION_MS 4000
#define DOUBLE_RESET_WINDOW_MS 3000
#define STATION_TABLE_SIZE 8
#define SERIAL_COMMAND_IDLE_MS 200 // a command without a line ending ends after this pause
#define SERIAL_COMMAND_MAX_CHARS 64

// Flash storage (ESP32 Preferences). Names are limited to 15 characters.
#define PREFS_NAMESPACE "aprs_hunter"
#define PREFS_KEY_DISPLAY "display"
#define PREFS_KEY_RESET_FLAG "dbl_reset"

SX1278 radio = new Module(LORA_SS, LORA_DIO0, LORA_RST, LORA_DIO1);

volatile bool packetReady = false;
volatile uint32_t packetUptimeMs = 0;

uint32_t packetCount = 0;
NoiseStats noise;
uint32_t lastNoiseSampleMs = 0;
uint32_t lastNoiseReportMs = 0;
uint32_t ledOnSinceMs = 0;
bool ledOn = false;

/**
 * The double-reset flag, kept in flash so it survives the reset button.
 */
class PreferencesFlagStore : public FlagStore {
  public:
    explicit PreferencesFlagStore(Preferences &preferences) : prefs(preferences) {}
    bool read() override { return prefs.getBool(PREFS_KEY_RESET_FLAG, false); }
    void write(bool value) override { prefs.putBool(PREFS_KEY_RESET_FLAG, value); }

  private:
    Preferences &prefs;
};

Preferences prefs;
PreferencesFlagStore resetFlag(prefs);
DoubleResetDetector doubleReset(resetFlag, DOUBLE_RESET_WINDOW_MS);

OledView oled;
bool displayReady = false;
DisplayMode displayMode = DisplayMode::Animated;
StationTable stations(STATION_TABLE_SIZE);
PacketSummary lastPacket;
OneShotTimer splashTimer;
OneShotTimer notificationTimer;
uint32_t lastFrameMs = 0;

std::string serialCommand;
uint32_t lastSerialCharMs = 0;

/**
 * DIO0 interrupt: the radio has received a packet. Only note the time here;
 * loop() reads the packet.
 */
void IRAM_ATTR onPacketReceived() {
  packetUptimeMs = millis();
  packetReady = true;
}

void printLine(const std::string &line) {
  Serial.println(line.c_str());
}

RadioSettings aprsRadioSettings() {
  RadioSettings settings;
  settings.frequencyHz = APRS_FREQUENCY_HZ;
  settings.bandwidthHz = APRS_BANDWIDTH_HZ;
  settings.spreadingFactor = APRS_SPREADING_FACTOR;
  settings.codingRate = APRS_CODING_RATE;
  settings.syncWord = APRS_SYNC_WORD;
  settings.preambleLength = APRS_PREAMBLE_LENGTH;
  return settings;
}

/**
 * Configure the radio and start continuous receive.
 *
 * @return RADIOLIB_ERR_NONE, or the RadioLib error code of the step that failed
 */
int16_t startRadio(const RadioSettings &settings) {
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_SS);

  ConfigLoRa_t config;
  config.frequency = settings.frequencyHz;
  config.bandwidth = settings.bandwidthHz;
  config.spreadingFactor = settings.spreadingFactor;
  config.codingRate = settings.codingRate;
  config.syncWord = settings.syncWord;
  config.preambleLength = settings.preambleLength;

  int16_t state = radio.begin(config);
  if (state != RADIOLIB_ERR_NONE) {
    return state;
  }
  state = radio.setCRC(true);
  if (state != RADIOLIB_ERR_NONE) {
    return state;
  }
  radio.setDio0Action(onPacketReceived, RISING);
  return radio.startReceive();
}

/**
 * Show a notification for a packet, and add its station to the sonar if the
 * packet is a valid LoRa APRS frame that passed the CRC check.
 */
void notifyPacket(const RxRecord &record) {
  lastPacket = summarizePacket(record);
  if (lastPacket.frameValid && record.crcOk) {
    stations.heard(lastPacket.callsign, record.rssiDbm, record.uptimeMs);
  }
  notificationTimer.start(millis(), NOTIFICATION_MS);
}

/**
 * Read the packet the radio has just received and print its record.
 * Packets that fail the CRC check are printed too, with crc_ok false.
 */
void handlePacket(uint32_t uptimeMs) {
  RxRecord record;
  record.seq = ++packetCount;
  record.uptimeMs = uptimeMs;
  record.frequencyHz = APRS_FREQUENCY_HZ;

  // Read the status registers first: readData() clears the IRQ flags.
  record.irqFlags = (uint8_t)radio.getIRQFlags();
  record.modemStatus = radio.getModemStatus();
  record.rssiDbm = radio.getRSSI();
  record.snrDb = radio.getSNR();
  record.frequencyErrorHz = radio.getFrequencyError();

  size_t length = radio.getPacketLength();
  if (length == 0) {
    printLine(formatErrorRecord(millis(), "radio.getPacketLength", 0));
    radio.startReceive();
    return;
  }

  record.data.resize(length);
  int16_t state = radio.readData(record.data.data(), length);
  record.crcOk = (state == RADIOLIB_ERR_NONE);
  record.timeOnAirUs = (uint32_t)radio.getTimeOnAir(length);

  if (state == RADIOLIB_ERR_NONE || state == RADIOLIB_ERR_CRC_MISMATCH) {
    printLine(formatRxRecord(record));
    notifyPacket(record);
  } else {
    printLine(formatErrorRecord(millis(), "radio.readData", state));
  }

  radio.startReceive();
}

void flashLed(uint32_t now) {
  digitalWrite(BOARD_LED, HIGH);
  ledOn = true;
  ledOnSinceMs = now;
}

void updateLed(uint32_t now) {
  if (ledOn && now - ledOnSinceMs >= LED_FLASH_MS) {
    digitalWrite(BOARD_LED, LOW);
    ledOn = false;
  }
}

/**
 * Switch the display mode, save it in flash, report it, and show the splash
 * so the person wearing the board sees the change.
 */
void setDisplayMode(DisplayMode mode, const char *source) {
  displayMode = mode;
  prefs.putUChar(PREFS_KEY_DISPLAY, (uint8_t)mode);
  printLine(formatDisplayRecord(millis(), displayModeName(mode), source));
  splashTimer.start(millis(), SPLASH_MS);
}

void handleSerialCommand(const std::string &command) {
  DisplayMode mode;
  if (parseDisplayCommand(command, displayMode, mode)) {
    setDisplayMode(mode, "serial");
  } else {
    printLine(formatErrorRecord(millis(), "serial: unknown command", 0));
  }
}

/**
 * Collect serial input into a command. A command ends at a line ending, or
 * after a short pause when the serial monitor sends no line ending.
 */
void readSerialCommands(uint32_t now) {
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    lastSerialCharMs = now;
    if (c == '\n' || c == '\r') {
      if (!serialCommand.empty()) {
        handleSerialCommand(serialCommand);
        serialCommand.clear();
      }
    } else if (serialCommand.size() < SERIAL_COMMAND_MAX_CHARS) {
      serialCommand += c;
    }
  }
  if (!serialCommand.empty() && now - lastSerialCharMs >= SERIAL_COMMAND_IDLE_MS) {
    handleSerialCommand(serialCommand);
    serialCommand.clear();
  }
}

/**
 * Draw the next frame. Each screen is drawn before the display turns on,
 * so a stale picture never flashes up.
 */
void updateDisplay(uint32_t now) {
  if (!displayReady || now - lastFrameMs < DISPLAY_FRAME_MS) {
    return;
  }
  lastFrameMs = now;

  switch (chooseScreen(displayMode, splashTimer.active(now), notificationTimer.active(now))) {
    case Screen::Off:
      oled.setPower(false);
      break;
    case Screen::Splash:
      oled.drawSplash(displayMode);
      oled.setPower(true);
      break;
    case Screen::Notification:
      oled.drawNotification(lastPacket, notificationTimer.elapsed(now));
      oled.setPower(true);
      break;
    case Screen::Sonar:
      oled.drawSonar(stations, now, packetCount);
      oled.setPower(true);
      break;
  }
}

void setup() {
  pinMode(BOARD_LED, OUTPUT);
  digitalWrite(BOARD_LED, LOW);
  Serial.begin(SERIAL_BAUD);

  prefs.begin(PREFS_NAMESPACE, false);
  displayMode = displayModeFromByte(prefs.getUChar(PREFS_KEY_DISPLAY, (uint8_t)DisplayMode::Animated));
  bool switchedByReset = doubleReset.detect();
  if (switchedByReset) {
    displayMode = toggledMode(displayMode);
    prefs.putUChar(PREFS_KEY_DISPLAY, (uint8_t)displayMode);
  }

  RadioSettings settings = aprsRadioSettings();
  int16_t state = startRadio(settings);
  printLine(formatBootRecord(FIRMWARE_VERSION, millis(), (int)esp_reset_reason(), settings, state,
                             displayModeName(displayMode)));
  if (switchedByReset) {
    printLine(formatDisplayRecord(millis(), displayModeName(displayMode), "double_reset"));
  }

  displayReady = oled.begin(OLED_SDA, OLED_SCL, DISPLAY_CONTRAST, DISPLAY_ROTATION);
  if (!displayReady) {
    printLine(formatErrorRecord(millis(), "display.begin", 0));
  }
  splashTimer.start(millis(), SPLASH_MS);

  if (state != RADIOLIB_ERR_NONE) {
    // Keep reporting, so the logger shows why no packets arrive.
    while (true) {
      printLine(formatErrorRecord(millis(), "radio.begin", state));
      delay(5000);
    }
  }

  lastNoiseSampleMs = millis();
  lastNoiseReportMs = lastNoiseSampleMs;
}

void loop() {
  if (packetReady) {
    uint32_t uptimeMs = packetUptimeMs;
    packetReady = false;
    handlePacket(uptimeMs);
    flashLed(millis());
  }

  uint32_t now = millis();
  doubleReset.update(now);
  readSerialCommands(now);

  if (now - lastNoiseSampleMs >= NOISE_SAMPLE_INTERVAL_MS) {
    lastNoiseSampleMs = now;
    noise.add(radio.getRSSI(false)); // current channel RSSI
  }
  if (now - lastNoiseReportMs >= NOISE_REPORT_INTERVAL_MS) {
    lastNoiseReportMs = now;
    printLine(formatNoiseRecord(now, APRS_FREQUENCY_HZ, noise));
    noise.reset();
  }

  updateLed(now);
  updateDisplay(now);
}
