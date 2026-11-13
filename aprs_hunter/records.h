/**
 * JSON Lines records that the board prints over USB serial.
 *
 * Every record is one JSON object on one line, with a schema version "v"
 * and a "type": "rx" (a received packet), "noise" (a noise floor summary),
 * "boot" (start-up settings), "display" (a display mode change),
 * "rssi_trace" (the raw RSSI reads around a jump), or "error".
 */

#ifndef RECORDS_H
#define RECORDS_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "rssi_trace.h"
#include "stable_read.h"

/**
 * Radio settings, reported once in the boot record.
 */
struct RadioSettings
{
    uint32_t frequencyHz = 0;
    uint32_t bandwidthHz = 0;
    uint8_t spreadingFactor = 0;
    uint8_t codingRate = 0; // denominator: 5 means 4/5
    uint8_t syncWord = 0;
    uint16_t preambleLength = 0;
};

/**
 * Everything the board knows about one received packet.
 */
struct RxRecord
{
    uint32_t seq = 0;      // packet number since boot, starting at 1
    uint32_t uptimeMs = 0; // when the radio signalled the packet
    uint32_t frequencyHz = 0;
    float rssiDbm = 0.0f;
    float snrDb = 0.0f;
    int32_t frequencyErrorHz = 0;
    bool crcOk = false;
    uint8_t irqFlags = 0;    // SX127x RegIrqFlags (0x12), read before it is cleared
    uint8_t modemStatus = 0; // SX127x RegModemStat (0x18)
    uint32_t timeOnAirUs = 0;
    std::vector<uint8_t> data;
};

/**
 * Running summary of noise floor samples.
 */
class NoiseStats
{
  public:
    void add(float rssiDbm);

    /**
     * Add a checked reading: its value if the reads agreed, else one skipped
     * sample. Its mismatches add to the window's read mismatches.
     */
    void addReading(const StableRead &reading);

    void reset();
    uint32_t count() const;
    float minDbm() const;
    float maxDbm() const;
    float meanDbm() const;
    uint32_t readMismatches() const;
    uint32_t skippedSamples() const;

  private:
    uint32_t samples = 0;
    float minimum = 0.0f;
    float maximum = 0.0f;
    double sum = 0.0;
    uint32_t mismatches = 0;
    uint32_t skipped = 0;
};

/**
 * Escape bytes for a JSON string. Printable ASCII stays as it is. Quotes and
 * backslashes get a backslash. Every other byte becomes \u00XX, so the
 * output is always valid JSON. Use the raw hex for the exact bytes.
 */
std::string jsonEscape(const std::string &bytes);

/**
 * Write bytes as lowercase hex.
 */
std::string toHex(const uint8_t *data, size_t length);

/**
 * Coding rate denominator of the last received LoRa header.
 *
 * @param modemStatus SX127x RegModemStat; bits 7-5 hold the coding rate
 * @return 5 to 8 for 4/5 to 4/8, or 0 if the field is empty
 */
int receivedCodingRate(uint8_t modemStatus);

std::string formatRxRecord(const RxRecord &record);
std::string formatNoiseRecord(uint32_t uptimeMs, uint32_t frequencyHz, const NoiseStats &stats);
std::string formatBootRecord(const char *firmwareVersion, uint32_t uptimeMs, int resetReason,
                             const RadioSettings &radio, int radioStatus, const char *displayMode);
std::string formatErrorRecord(uint32_t uptimeMs, const char *where, int code);

/**
 * The raw RSSI reads of one noise sample that jumped.
 *
 * @param referenceDbm the noise mean the reads were compared with
 * @param reading what the stable read returned
 * @param stableReads how many of the reads the stable read made; the rest
 *        came after it
 * @param missed traces refused since the last one printed
 */
std::string formatTraceRecord(uint32_t uptimeMs, float referenceDbm, const StableRead &reading,
                              size_t stableReads, uint32_t missed, const TraceBuffer &reads);

/**
 * A display mode change.
 *
 * @param mode the new mode name, "animated" or "notify"
 * @param source what changed it, "serial" or "double_reset"
 */
std::string formatDisplayRecord(uint32_t uptimeMs, const char *mode, const char *source);

#endif
