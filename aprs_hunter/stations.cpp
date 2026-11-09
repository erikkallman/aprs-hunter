#include "stations.h"

StationTable::StationTable(size_t maxStations) : capacity(maxStations)
{
}

void StationTable::heard(const std::string &callsign, float rssiDbm, uint32_t nowMs)
{
    StationEntry entry{callsign, rssiDbm, nowMs, 1};
    for (auto it = stations.begin(); it != stations.end(); ++it) {
        if (it->callsign == callsign) {
            entry.packets = it->packets + 1;
            stations.erase(it);
            break;
        }
    }
    stations.insert(stations.begin(), entry);
    if (stations.size() > capacity) {
        stations.pop_back();
    }
}

const std::vector<StationEntry> &StationTable::entries() const
{
    return stations;
}

bool heardWithin(const StationEntry &entry, uint32_t nowMs, uint32_t windowMs)
{
    return nowMs - entry.lastHeardMs < windowMs; // unsigned subtraction handles the wrap
}
