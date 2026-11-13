#include "stable_read.h"

#include <cmath>

StableRead readStable(RssiSource &source, uint8_t maxReads)
{
    if (maxReads < 2) {
        maxReads = 2;
    }
    StableRead result;
    float previous = source.read();
    for (uint8_t reads = 1; reads < maxReads; reads++) {
        float current = source.read();
        // Both values are an offset plus a whole register value, so they
        // compare exactly.
        if (current == previous) {
            result.ok = true;
            result.value = current;
            return result;
        }
        result.mismatches++;
        previous = current;
    }
    return result;
}

StableRead confirmFarRead(const StableRead &reading, RssiSource &shifted, float referenceDbm,
                          const ConfirmRule &rule)
{
    if (!reading.ok || std::fabs(reading.value - referenceDbm) < rule.farDb) {
        return reading;
    }
    if (std::fabs(shifted.read() - reading.value) <= rule.toleranceDb) {
        return reading;
    }
    StableRead rejected;
    rejected.mismatches = reading.mismatches + 1;
    rejected.unconfirmed = true;
    return rejected;
}
