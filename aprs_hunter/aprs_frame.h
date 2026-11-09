/**
 * LoRa APRS frame parsing.
 *
 * A LoRa APRS frame is the 3-byte prefix 3C FF 01 followed by an APRS
 * packet in TNC2 text form: SOURCE>DESTINATION,PATH1,PATH2:INFO
 *
 * This parser only splits the header. Full APRS decoding (positions,
 * weather, Mic-E) happens on the base computer.
 */

#ifndef APRS_FRAME_H
#define APRS_FRAME_H

#include <cstddef>
#include <cstdint>
#include <string>

struct AprsFrame
{
    bool hasPrefix = false; // the frame starts with 3C FF 01
    bool valid = false;     // the prefix and the header were found
    std::string source;
    std::string destination;
    std::string path;  // comma-separated digipeater path; empty if none
    std::string info;  // everything after the first ':'; may hold any byte
    std::string error; // why the frame is invalid; empty when valid
};

/**
 * Split a received LoRa APRS frame into its header fields and info.
 *
 * @param data received bytes
 * @param length number of received bytes
 * @return the parsed frame; check valid and error
 */
AprsFrame parseLoRaAprsFrame(const uint8_t *data, size_t length);

#endif
