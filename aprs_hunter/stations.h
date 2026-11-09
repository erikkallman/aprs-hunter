/**
 * Recently heard stations, for the sonar screen.
 */

#ifndef STATIONS_H
#define STATIONS_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct StationEntry
{
    std::string callsign;
    float rssiDbm;        // signal of the last packet
    uint32_t lastHeardMs; // uptime of the last packet
    uint32_t packets;     // packets heard since boot
};

/**
 * A small table of stations, most recently heard first. When it is full, a
 * new station replaces the one heard longest ago.
 */
class StationTable
{
  public:
    explicit StationTable(size_t capacity);
    void heard(const std::string &callsign, float rssiDbm, uint32_t nowMs);
    const std::vector<StationEntry> &entries() const;

  private:
    size_t capacity;
    std::vector<StationEntry> stations;
};

/** true if the station was heard less than windowMs ago. */
bool heardWithin(const StationEntry &entry, uint32_t nowMs, uint32_t windowMs);

#endif
