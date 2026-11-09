#include "double_reset.h"

DoubleResetDetector::DoubleResetDetector(FlagStore &flagStore, uint32_t window)
    : store(flagStore), windowMs(window)
{
}

bool DoubleResetDetector::detect()
{
    if (store.read()) {
        store.write(false);
        return true;
    }
    store.write(true);
    armed = true;
    return false;
}

void DoubleResetDetector::update(uint32_t uptimeMs)
{
    if (armed && uptimeMs >= windowMs) {
        store.write(false);
        armed = false;
    }
}
