#include "records.h"

#include <cstdio>

#include "aprs_frame.h"

// ---- NoiseStats ----

void NoiseStats::add(float rssiDbm)
{
    if (samples == 0 || rssiDbm < minimum) {
        minimum = rssiDbm;
    }
    if (samples == 0 || rssiDbm > maximum) {
        maximum = rssiDbm;
    }
    sum += rssiDbm;
    samples++;
}

void NoiseStats::reset()
{
    samples = 0;
    minimum = 0.0f;
    maximum = 0.0f;
    sum = 0.0;
}

uint32_t NoiseStats::count() const
{
    return samples;
}

float NoiseStats::minDbm() const
{
    return minimum;
}

float NoiseStats::maxDbm() const
{
    return maximum;
}

float NoiseStats::meanDbm() const
{
    return samples == 0 ? 0.0f : static_cast<float>(sum / samples);
}

// ---- Encoding helpers ----

std::string jsonEscape(const std::string &bytes)
{
    std::string out;
    out.reserve(bytes.size());
    for (unsigned char c : bytes) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c >= 0x20 && c <= 0x7E) {
            out += static_cast<char>(c);
        } else {
            char escaped[7];
            std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
            out += escaped;
        }
    }
    return out;
}

std::string toHex(const uint8_t *data, size_t length)
{
    static const char DIGITS[] = "0123456789abcdef";
    std::string out;
    out.reserve(length * 2);
    for (size_t i = 0; i < length; i++) {
        out += DIGITS[data[i] >> 4];
        out += DIGITS[data[i] & 0x0F];
    }
    return out;
}

int receivedCodingRate(uint8_t modemStatus)
{
    int field = (modemStatus >> 5) & 0x07;
    if (field < 1 || field > 4) {
        return 0;
    }
    return 4 + field;
}

// ---- JSON building ----

namespace
{
    void appendKey(std::string &out, const char *key)
    {
        out += ",\"";
        out += key;
        out += "\":";
    }

    void appendUnsigned(std::string &out, const char *key, unsigned long value)
    {
        appendKey(out, key);
        out += std::to_string(value);
    }

    void appendSigned(std::string &out, const char *key, long value)
    {
        appendKey(out, key);
        out += std::to_string(value);
    }

    void appendFloat(std::string &out, const char *key, float value)
    {
        char text[24];
        std::snprintf(text, sizeof(text), "%.2f", static_cast<double>(value));
        appendKey(out, key);
        out += text;
    }

    void appendNull(std::string &out, const char *key)
    {
        appendKey(out, key);
        out += "null";
    }

    void appendBool(std::string &out, const char *key, bool value)
    {
        appendKey(out, key);
        out += value ? "true" : "false";
    }

    void appendString(std::string &out, const char *key, const std::string &value)
    {
        appendKey(out, key);
        out += '"';
        out += jsonEscape(value);
        out += '"';
    }

    std::string recordStart(const char *type)
    {
        return std::string("{\"v\":1,\"type\":\"") + type + "\"";
    }
}

std::string formatRxRecord(const RxRecord &record)
{
    std::string out = recordStart("rx");
    appendUnsigned(out, "seq", record.seq);
    appendUnsigned(out, "uptime_ms", record.uptimeMs);
    appendUnsigned(out, "freq_hz", record.frequencyHz);
    appendFloat(out, "rssi_dbm", record.rssiDbm);
    appendFloat(out, "snr_db", record.snrDb);
    appendSigned(out, "freq_err_hz", record.frequencyErrorHz);
    appendBool(out, "crc_ok", record.crcOk);

    int codingRate = receivedCodingRate(record.modemStatus);
    if (codingRate == 0) {
        appendNull(out, "rx_cr");
    } else {
        appendUnsigned(out, "rx_cr", codingRate);
    }

    appendUnsigned(out, "irq", record.irqFlags);
    appendUnsigned(out, "modem_stat", record.modemStatus);
    appendUnsigned(out, "toa_us", record.timeOnAirUs);
    appendUnsigned(out, "len", record.data.size());
    appendKey(out, "raw_hex");
    out += '"' + toHex(record.data.data(), record.data.size()) + '"';

    AprsFrame frame = parseLoRaAprsFrame(record.data.data(), record.data.size());
    if (frame.valid) {
        out += ",\"aprs\":{\"src\":\"" + jsonEscape(frame.source) + "\"";
        appendString(out, "dst", frame.destination);
        appendString(out, "path", frame.path);
        appendString(out, "info", frame.info);
        out += '}';
    } else {
        appendNull(out, "aprs");
        appendString(out, "aprs_error", frame.error);
    }

    out += '}';
    return out;
}

std::string formatNoiseRecord(uint32_t uptimeMs, uint32_t frequencyHz, const NoiseStats &stats)
{
    std::string out = recordStart("noise");
    appendUnsigned(out, "uptime_ms", uptimeMs);
    appendUnsigned(out, "freq_hz", frequencyHz);
    appendUnsigned(out, "samples", stats.count());
    if (stats.count() == 0) {
        appendNull(out, "min_dbm");
        appendNull(out, "mean_dbm");
        appendNull(out, "max_dbm");
    } else {
        appendFloat(out, "min_dbm", stats.minDbm());
        appendFloat(out, "mean_dbm", stats.meanDbm());
        appendFloat(out, "max_dbm", stats.maxDbm());
    }
    out += '}';
    return out;
}

std::string formatBootRecord(const char *firmwareVersion, uint32_t uptimeMs, int resetReason,
                             const RadioSettings &radio, int radioStatus)
{
    std::string out = recordStart("boot");
    appendString(out, "fw", firmwareVersion);
    appendUnsigned(out, "uptime_ms", uptimeMs);
    appendSigned(out, "reset_reason", resetReason);
    appendSigned(out, "radio_status", radioStatus);
    out += ",\"radio\":{\"freq_hz\":" + std::to_string(radio.frequencyHz);
    appendUnsigned(out, "bw_hz", radio.bandwidthHz);
    appendUnsigned(out, "sf", radio.spreadingFactor);
    appendUnsigned(out, "cr", radio.codingRate);
    appendUnsigned(out, "sync_word", radio.syncWord);
    appendUnsigned(out, "preamble", radio.preambleLength);
    out += "}}";
    return out;
}

std::string formatErrorRecord(uint32_t uptimeMs, const char *where, int code)
{
    std::string out = recordStart("error");
    appendUnsigned(out, "uptime_ms", uptimeMs);
    appendString(out, "where", where);
    appendSigned(out, "code", code);
    out += '}';
    return out;
}
