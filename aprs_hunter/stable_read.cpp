#include "stable_read.h"

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
