#ifndef TEST_HELPERS_H
#define TEST_HELPERS_H

#include <cstdint>
#include <string>
#include <vector>

/**
 * Build the bytes of a LoRa APRS frame from its text.
 *
 * @param text TNC2 text, for example "N0CALL>APRS:>hello"
 * @param withPrefix true to put the 3C FF 01 prefix in front
 */
inline std::vector<uint8_t> loraAprsFrame(const std::string &text, bool withPrefix = true)
{
    std::vector<uint8_t> bytes;
    if (withPrefix) {
        bytes = {0x3C, 0xFF, 0x01};
    }
    bytes.insert(bytes.end(), text.begin(), text.end());
    return bytes;
}

#endif
