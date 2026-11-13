#include "rssi_trace.h"

#include <cmath>

void TraceBuffer::clear()
{
    count = 0;
}

void TraceBuffer::add(float dbm, uint32_t us)
{
    if (count >= TRACE_CAPACITY) {
        return;
    }
    reads[count].dbm = dbm;
    reads[count].us = us;
    count++;
}

size_t TraceBuffer::size() const
{
    return count;
}

const TraceRead &TraceBuffer::at(size_t index) const
{
    return reads[index];
}

bool hasJump(const TraceBuffer &reads, float referenceDbm, float jumpDb)
{
    for (size_t i = 0; i < reads.size(); i++) {
        if (std::fabs(reads.at(i).dbm - referenceDbm) >= jumpDb) {
            return true;
        }
    }
    return false;
}

TraceGate::TraceGate(uint32_t intervalMs, uint32_t limit)
    : minIntervalMs(intervalMs), maxTraces(limit)
{
}

bool TraceGate::allow(uint32_t nowMs)
{
    if (allowed >= maxTraces) {
        return false;
    }
    // Unsigned subtraction stays right when the millisecond counter wraps.
    if (allowed > 0 && nowMs - lastAllowedMs < minIntervalMs) {
        refused++;
        return false;
    }
    allowed++;
    lastAllowedMs = nowMs;
    refusedBeforeLast = refused;
    refused = 0;
    return true;
}

uint32_t TraceGate::missedBefore() const
{
    return refusedBeforeLast;
}
