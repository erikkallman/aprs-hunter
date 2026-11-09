#include "aprs_frame.h"

static const uint8_t LORA_APRS_PREFIX[] = {0x3C, 0xFF, 0x01};
static const size_t LORA_APRS_PREFIX_LENGTH = sizeof(LORA_APRS_PREFIX);

AprsFrame parseLoRaAprsFrame(const uint8_t *data, size_t length)
{
    AprsFrame frame;

    if (length < LORA_APRS_PREFIX_LENGTH) {
        frame.error = "shorter than the LoRa APRS prefix";
        return frame;
    }
    for (size_t i = 0; i < LORA_APRS_PREFIX_LENGTH; i++) {
        if (data[i] != LORA_APRS_PREFIX[i]) {
            frame.error = "no LoRa APRS prefix";
            return frame;
        }
    }
    frame.hasPrefix = true;

    std::string text(reinterpret_cast<const char *>(data) + LORA_APRS_PREFIX_LENGTH,
                     length - LORA_APRS_PREFIX_LENGTH);

    // The header ends at the first ':'. The info may hold more ':' and '>'.
    size_t colon = text.find(':');
    if (colon == std::string::npos) {
        frame.error = "no ':' after the header";
        return frame;
    }
    std::string header = text.substr(0, colon);

    size_t arrow = header.find('>');
    if (arrow == std::string::npos) {
        frame.error = "no '>' in the header";
        return frame;
    }
    if (arrow == 0) {
        frame.error = "empty source";
        return frame;
    }

    frame.source = header.substr(0, arrow);
    std::string addresses = header.substr(arrow + 1);
    size_t comma = addresses.find(',');
    if (comma == std::string::npos) {
        frame.destination = addresses;
    } else {
        frame.destination = addresses.substr(0, comma);
        frame.path = addresses.substr(comma + 1);
    }
    frame.info = text.substr(colon + 1);
    frame.valid = true;
    return frame;
}
