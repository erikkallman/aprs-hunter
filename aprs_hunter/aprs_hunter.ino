/**
 * APRS hunter: a receive-only LoRa APRS logger for the LilyGO T3 V1.6.1
 * (433 MHz version, SX1278 radio).
 *
 * The sketch prints one JSON object per line over USB serial at 115200 baud:
 *   boot  - firmware version and radio settings, once at start-up
 *   rx    - every received packet, with signal metadata and the raw bytes
 *   noise - a noise floor summary every 10 seconds
 *   error - radio errors
 *
 * This sketch never transmits. The board LED flashes on every packet.
 */

#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>
#include <esp_system.h>

#include "records.h"

#define FIRMWARE_VERSION "0.1.0"

// LilyGO T3 V1.6.1 radio pins (same on the 433 MHz and 868 MHz versions)
#define LORA_SCK 5
#define LORA_MISO 19
#define LORA_MOSI 27
#define LORA_SS 18
#define LORA_RST 23
#define LORA_DIO0 26
#define LORA_DIO1 33

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

void setup() {
  pinMode(BOARD_LED, OUTPUT);
  digitalWrite(BOARD_LED, LOW);
  Serial.begin(SERIAL_BAUD);

  RadioSettings settings = aprsRadioSettings();
  int16_t state = startRadio(settings);
  printLine(formatBootRecord(FIRMWARE_VERSION, millis(), (int)esp_reset_reason(), settings, state));

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
}
