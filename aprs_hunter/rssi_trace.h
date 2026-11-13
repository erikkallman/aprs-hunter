/**
 * A trace of the raw RSSI reads around a jump, to measure how long a torn
 * value stays in the radio's RSSI register.
 *
 * Two reads in a row sometimes agree on a torn value (see stable_read.h), so
 * the mixed value must sit in the register for a while. The sketch records
 * every read of a noise sample with its time. When one read lands far from
 * the last noise mean, it makes more reads and prints them all, so the log
 * shows how many reads the bad value lasted.
 */

#ifndef RSSI_TRACE_H
#define RSSI_TRACE_H

#include <cstddef>
#include <cstdint>

// Most reads one trace holds: the stable read plus the reads after it.
const size_t TRACE_CAPACITY = 32;

struct TraceRead
{
    float dbm = 0.0f;
    uint32_t us = 0; // micros() when the read finished
};

/**
 * The reads of one noise sample, in order.
 */
class TraceBuffer
{
  public:
    void clear();

    /** Add a read. Reads past TRACE_CAPACITY are ignored. */
    void add(float dbm, uint32_t us);

    size_t size() const;
    const TraceRead &at(size_t index) const;

  private:
    TraceRead reads[TRACE_CAPACITY];
    size_t count = 0;
};

/**
 * True when a read is at least jumpDb above or below the reference.
 */
bool hasJump(const TraceBuffer &reads, float referenceDbm, float jumpDb);

/**
 * Limits how many traces the board prints: at most one per interval and
 * maxTraces in all, so a real signal cannot flood the serial line.
 */
class TraceGate
{
  public:
    TraceGate(uint32_t minIntervalMs, uint32_t maxTraces);

    /**
     * Ask to print a trace now. Counts the traces it refuses inside the
     * interval; traces past the limit are not counted.
     */
    bool allow(uint32_t nowMs);

    /** Traces refused inside the interval before the last one allowed. */
    uint32_t missedBefore() const;

  private:
    uint32_t minIntervalMs;
    uint32_t maxTraces;
    uint32_t allowed = 0;
    uint32_t lastAllowedMs = 0;
    uint32_t refused = 0;
    uint32_t refusedBeforeLast = 0;
};

#endif
