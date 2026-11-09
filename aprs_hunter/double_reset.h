/**
 * Double reset detection: pressing reset twice within a short window counts
 * as a command. The T3 V1.6.1 has no user button, so this is the only input
 * the board has away from a computer.
 *
 * How it works: at boot the detector sets a flag in storage that survives a
 * reset (flash). If the board stays up for the whole window, the flag is
 * cleared. If the board resets inside the window, the next boot finds the
 * flag still set: that is a double reset.
 */

#ifndef DOUBLE_RESET_H
#define DOUBLE_RESET_H

#include <cstdint>

/**
 * One boolean that survives a reset. The sketch implements it with ESP32
 * Preferences; tests use a fake.
 */
class FlagStore
{
  public:
    virtual ~FlagStore() = default;
    virtual bool read() = 0;
    virtual void write(bool value) = 0;
};

class DoubleResetDetector
{
  public:
    DoubleResetDetector(FlagStore &store, uint32_t windowMs);

    /** Call once at boot. Returns true if this boot is a double reset. */
    bool detect();

    /** Call from loop(). Clears the flag once the window has passed. */
    void update(uint32_t uptimeMs);

  private:
    FlagStore &store;
    uint32_t windowMs;
    bool armed = false;
};

#endif
