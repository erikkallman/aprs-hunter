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
 *
 * "Almost never" is not never. The register changes on a fixed 8 us clock
 * (one sample at 125 kHz bandwidth). When the time between two reads is close
 * to a multiple of 8 us, the clock tick lands on the same bit in both reads,
 * and both come out as the same torn value. So a value far from the noise
 * floor needs one more read, made after a short pause. The pause moves the
 * tick to another bit, so a torn value cannot repeat a third time. A real
 * signal lasts and does repeat. See AUDIT_2026-11-13_rssi_read_spacing.md.
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
    bool unconfirmed = false; // two reads agreed on a far value, the confirm read did not
};

/**
 * When an agreed value needs a confirm read, and when that read confirms it.
 */
struct ConfirmRule
{
    float farDb;       // a value at least this far from the reference needs a confirm read
    float toleranceDb; // the confirm read confirms when it is within this of the value
};

/**
 * Read until two reads in a row agree, or until maxReads reads.
 *
 * @param maxReads the most reads to make; values below 2 count as 2
 */
StableRead readStable(RssiSource &source, uint8_t maxReads);

/**
 * Check an agreed value that lies far from the reference with one more read.
 *
 * A reading that is not ok, or whose value is near the reference, comes back
 * as it is, and the source is not read. A far value that the confirm read
 * does not repeat comes back not ok, marked unconfirmed, with one more
 * mismatch.
 *
 * The confirm read needs only to be near the value, not equal to it: the
 * register of a real signal moves a decibel or so between reads.
 *
 * @param shifted a source that pauses before it reads, so that a register
 *        clock tick cannot land on the same bit as in the reads before
 * @param referenceDbm the noise floor to measure "far" from
 */
StableRead confirmFarRead(const StableRead &reading, RssiSource &shifted, float referenceDbm,
                          const ConfirmRule &rule);

#endif
