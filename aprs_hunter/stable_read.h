/**
 * Reading the RSSI register so that a torn read never counts.
 *
 * The SX1278 updates its current RSSI register all the time. SPI shifts the
 * byte out one bit at a time, top bit first. If the register changes during
 * the read, the board gets the top bits of the old value and the bottom bits
 * of the new one. Around -100 dBm the register flips between 0x3F and 0x40,
 * and seven bits change at once, so a -101 dBm reading can come out as
 * -164 dBm or -37 dBm.
 *
 * A torn read almost never matches the read after it. So a value counts only
 * when two reads in a row agree.
 */

#ifndef STABLE_READ_H
#define STABLE_READ_H

#include <cstdint>

/**
 * One reading of the current RSSI in dBm. The sketch reads the radio; tests
 * use a fake.
 */
class RssiSource
{
  public:
    virtual ~RssiSource() = default;
    virtual float read() = 0;
};

struct StableRead
{
    bool ok = false;        // two reads in a row agreed
    float value = 0.0f;     // the value they agreed on; 0 when ok is false
    uint8_t mismatches = 0; // how often a read disagreed with the read before it
};

/**
 * Read until two reads in a row agree, or until maxReads reads.
 *
 * @param maxReads the most reads to make; values below 2 count as 2
 */
StableRead readStable(RssiSource &source, uint8_t maxReads);

#endif
