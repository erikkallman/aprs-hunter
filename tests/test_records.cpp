#include "doctest.h"
#include "records.h"
#include "test_helpers.h"

namespace
{
    // "N0CALL>APRS:>hi" with the LoRa APRS prefix, as hex.
    const char *SHORT_FRAME_HEX = "3cff014e3043414c4c3e415052533a3e6869";

    RxRecord sampleRx(const std::vector<uint8_t> &data)
    {
        RxRecord record;
        record.seq = 3;
        record.uptimeMs = 123456;
        record.frequencyHz = 433775000;
        record.rssiDbm = -112.5f;
        record.snrDb = -7.25f;
        record.frequencyErrorHz = -1234;
        record.crcOk = true;
        record.irqFlags = 0x50;    // RxDone and ValidHeader
        record.modemStatus = 0x24; // coding rate field = 1, meaning 4/5
        record.timeOnAirUs = 1155072;
        record.data = data;
        return record;
    }

    RadioSettings aprsRadio()
    {
        RadioSettings radio;
        radio.frequencyHz = 433775000;
        radio.bandwidthHz = 125000;
        radio.spreadingFactor = 12;
        radio.codingRate = 5;
        radio.syncWord = 0x12;
        radio.preambleLength = 8;
        return radio;
    }
}

TEST_CASE("jsonEscape keeps printable ASCII")
{
    CHECK(jsonEscape("N0CALL>APRS:!hi") == "N0CALL>APRS:!hi");
}

TEST_CASE("jsonEscape escapes quotes and backslashes")
{
    CHECK(jsonEscape("a\"b\\c") == "a\\\"b\\\\c");
}

TEST_CASE("jsonEscape writes control bytes as \\u00XX")
{
    CHECK(jsonEscape(std::string("a\nb\0c", 5)) == "a\\u000ab\\u0000c");
}

TEST_CASE("jsonEscape writes non-ASCII bytes as \\u00XX")
{
    CHECK(jsonEscape("\xC3\xA5") == "\\u00c3\\u00a5");
}

TEST_CASE("toHex writes lowercase hex")
{
    const uint8_t bytes[] = {0x3C, 0xFF, 0x01, 0x0A};
    CHECK(toHex(bytes, sizeof(bytes)) == "3cff010a");
}

TEST_CASE("receivedCodingRate reads 4/5 from the modem status")
{
    CHECK(receivedCodingRate(0x24) == 5);
}

TEST_CASE("receivedCodingRate reads 4/8 from the modem status")
{
    CHECK(receivedCodingRate(0x80) == 8);
}

TEST_CASE("receivedCodingRate is 0 when no header was received")
{
    CHECK(receivedCodingRate(0x04) == 0);
}

TEST_CASE("A packet record with a valid APRS frame")
{
    std::string expected =
        "{\"v\":1,\"type\":\"rx\",\"seq\":3,\"uptime_ms\":123456,\"freq_hz\":433775000,"
        "\"rssi_dbm\":-112.50,\"snr_db\":-7.25,\"freq_err_hz\":-1234,\"crc_ok\":true,"
        "\"rx_cr\":5,\"irq\":80,\"modem_stat\":36,\"toa_us\":1155072,\"len\":18,"
        "\"raw_hex\":\"" + std::string(SHORT_FRAME_HEX) + "\","
        "\"aprs\":{\"src\":\"N0CALL\",\"dst\":\"APRS\",\"path\":\"\",\"info\":\">hi\"}}";
    CHECK(formatRxRecord(sampleRx(loraAprsFrame("N0CALL>APRS:>hi"))) == expected);
}

TEST_CASE("A packet record without the LoRa APRS prefix")
{
    RxRecord record = sampleRx({0x01, 0x02});
    record.crcOk = false;
    record.modemStatus = 0x04;
    std::string expected =
        "{\"v\":1,\"type\":\"rx\",\"seq\":3,\"uptime_ms\":123456,\"freq_hz\":433775000,"
        "\"rssi_dbm\":-112.50,\"snr_db\":-7.25,\"freq_err_hz\":-1234,\"crc_ok\":false,"
        "\"rx_cr\":null,\"irq\":80,\"modem_stat\":4,\"toa_us\":1155072,\"len\":2,"
        "\"raw_hex\":\"0102\",\"aprs\":null,\"aprs_error\":\"shorter than the LoRa APRS prefix\"}";
    CHECK(formatRxRecord(record) == expected);
}

TEST_CASE("NoiseStats counts samples")
{
    NoiseStats stats;
    stats.add(-120.0f);
    stats.add(-110.0f);
    stats.add(-115.0f);
    CHECK(stats.count() == 3);
}

TEST_CASE("NoiseStats gives the mean")
{
    NoiseStats stats;
    stats.add(-120.0f);
    stats.add(-110.0f);
    CHECK(stats.meanDbm() == doctest::Approx(-115.0f));
}

TEST_CASE("NoiseStats reset clears the samples")
{
    NoiseStats stats;
    stats.add(-120.0f);
    stats.reset();
    CHECK(stats.count() == 0);
}

TEST_CASE("A noise record")
{
    NoiseStats stats;
    stats.add(-120.0f);
    stats.add(-110.0f);
    stats.add(-115.0f);
    std::string expected =
        "{\"v\":1,\"type\":\"noise\",\"uptime_ms\":10000,\"freq_hz\":433775000,\"samples\":3,"
        "\"min_dbm\":-120.00,\"mean_dbm\":-115.00,\"max_dbm\":-110.00}";
    CHECK(formatNoiseRecord(10000, 433775000, stats) == expected);
}

TEST_CASE("A noise record with no samples has null values")
{
    NoiseStats stats;
    std::string expected =
        "{\"v\":1,\"type\":\"noise\",\"uptime_ms\":10000,\"freq_hz\":433775000,\"samples\":0,"
        "\"min_dbm\":null,\"mean_dbm\":null,\"max_dbm\":null}";
    CHECK(formatNoiseRecord(10000, 433775000, stats) == expected);
}

TEST_CASE("A boot record")
{
    std::string expected =
        "{\"v\":1,\"type\":\"boot\",\"fw\":\"0.1.0\",\"uptime_ms\":12,\"reset_reason\":1,"
        "\"radio_status\":0,\"radio\":{\"freq_hz\":433775000,\"bw_hz\":125000,\"sf\":12,"
        "\"cr\":5,\"sync_word\":18,\"preamble\":8}}";
    CHECK(formatBootRecord("0.1.0", 12, 1, aprsRadio(), 0) == expected);
}

TEST_CASE("An error record")
{
    std::string expected =
        "{\"v\":1,\"type\":\"error\",\"uptime_ms\":5000,\"where\":\"radio.readData\",\"code\":-2}";
    CHECK(formatErrorRecord(5000, "radio.readData", -2) == expected);
}
