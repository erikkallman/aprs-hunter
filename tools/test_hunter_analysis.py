"""Tests for hunter_analysis. Run with: make test-tools"""

import unittest

import hunter_analysis as ha


class RadioFlagTests(unittest.TestCase):
    def test_irq_flags_for_a_good_packet(self):
        self.assertEqual(ha.decode_irq_flags(0x50), ["RxDone", "ValidHeader"])

    def test_irq_flags_for_a_crc_failure(self):
        self.assertEqual(ha.decode_irq_flags(0x70), ["RxDone", "PayloadCrcError", "ValidHeader"])

    def test_irq_flags_for_none(self):
        self.assertEqual(ha.decode_irq_flags(None), [])

    def test_modem_status_coding_rate(self):
        self.assertEqual(ha.decode_modem_status(0x24)["rx_coding_rate"], "4/5")

    def test_modem_status_flags(self):
        self.assertEqual(ha.decode_modem_status(0x0B)["flags"], ["HeaderInfoValid", "SignalSynchronized", "SignalDetected"])

    def test_modem_status_without_a_coding_rate(self):
        self.assertIsNone(ha.decode_modem_status(0x04)["rx_coding_rate"])


class LinkMarginTests(unittest.TestCase):
    def test_margin_at_sf12(self):
        self.assertEqual(ha.link_margin_db(-7.25, 12), 12.75)

    def test_margin_at_sf9(self):
        self.assertEqual(ha.link_margin_db(-10.0, 9), 2.5)

    def test_margin_without_snr(self):
        self.assertIsNone(ha.link_margin_db(None, 12))


class HeardViaTests(unittest.TestCase):
    def test_no_used_digipeater_means_direct(self):
        self.assertEqual(ha.heard_via("WIDE1-1"), "direct")

    def test_empty_path_means_direct(self):
        self.assertEqual(ha.heard_via(""), "direct")

    def test_the_last_used_digipeater_is_the_one_heard(self):
        self.assertEqual(ha.heard_via("SM5ABC-10*,SM0XYZ-1*,WIDE2-1"), "SM0XYZ-1")

    def test_a_used_alias_counts_as_a_digipeater(self):
        self.assertEqual(ha.heard_via("WIDE1*,WIDE2-1"), "WIDE1")


class NoiseTests(unittest.TestCase):
    def test_a_register_zero_reading_is_a_glitch(self):
        self.assertTrue(ha.is_rssi_glitch(-164.0))

    def test_a_normal_reading_is_not_a_glitch(self):
        self.assertFalse(ha.is_rssi_glitch(-115.0))

    def rows(self, count):
        return [{"host_utc": "2026-11-09T12:%02d:00.000Z" % i, "samples": 94,
                 "min_dbm": -164.0 if i == 1 else -110.0, "mean_dbm": -100.0 - i,
                 "max_dbm": -90.0 + i, "rx_lat": None, "rx_lon": None} for i in range(count)]

    def checked_rows(self, count):
        """Rows from firmware 0.3.0 and later, which count dropped reads."""
        rows = self.rows(count)
        for i, row in enumerate(rows):
            row["read_mismatches"] = i
            row["skipped_samples"] = 1
            row["unconfirmed_samples"] = 2
        return rows

    def test_a_bucket_sums_the_read_mismatches(self):
        self.assertEqual(ha.downsample_noise(self.checked_rows(10), 5)[1]["read_mismatches"], 5)

    def test_a_bucket_sums_the_skipped_samples(self):
        self.assertEqual(ha.downsample_noise(self.checked_rows(10), 5)[1]["skipped_samples"], 2)

    def test_a_bucket_sums_the_unconfirmed_samples(self):
        self.assertEqual(ha.downsample_noise(self.checked_rows(10), 5)[1]["unconfirmed_samples"], 4)

    def test_a_bucket_from_older_firmware_has_no_read_mismatches(self):
        self.assertIsNone(ha.downsample_noise(self.rows(10), 5)[1]["read_mismatches"])

    def test_a_bucket_with_some_older_rows_sums_the_newer_rows(self):
        rows = self.rows(2) + self.checked_rows(2)
        self.assertEqual(ha.downsample_noise(rows, 1)[0]["read_mismatches"], 1)

    def test_few_rows_are_kept(self):
        self.assertEqual(len(ha.downsample_noise(self.rows(4), 10)), 4)

    def test_many_rows_are_reduced(self):
        self.assertEqual(len(ha.downsample_noise(self.rows(10), 5)), 5)

    def test_a_bucket_takes_the_lowest_valid_minimum(self):
        self.assertEqual(ha.downsample_noise(self.rows(10), 5)[0]["min_dbm"], -110.0)

    def test_a_bucket_counts_glitches(self):
        self.assertEqual(ha.downsample_noise(self.rows(10), 5)[0]["glitches"], 1)

    def test_a_bucket_averages_the_means(self):
        self.assertEqual(ha.downsample_noise(self.rows(10), 5)[0]["mean_dbm"], -100.5)

    def test_a_bucket_takes_the_highest_maximum(self):
        self.assertEqual(ha.downsample_noise(self.rows(10), 5)[0]["max_dbm"], -89.0)

    def test_a_kept_row_with_a_glitch_has_no_minimum(self):
        self.assertIsNone(ha.downsample_noise(self.rows(4), 10)[1]["min_dbm"])


def packet(src="N0CALL-7", rssi=-110.0, snr=-5.0, ferr=1000, crc_ok=1, host_utc="2026-11-09T12:00:00.000Z",
           path="WIDE1-1", distance=10.0, bearing=45.0, lat=59.4, lon=18.1, freq_hz=433775000):
    return {"src": src, "rssi_dbm": rssi, "snr_db": snr, "freq_err_hz": ferr, "crc_ok": crc_ok,
            "host_utc": host_utc, "path": path, "distance_km": distance, "bearing_deg": bearing,
            "latitude": lat, "longitude": lon, "freq_hz": freq_hz, "symbol_table": "/", "symbol": ">",
            "comment": "hello", "aprs_format": "uncompressed", "sf": 12}


class StationStatsTests(unittest.TestCase):
    def setUp(self):
        self.stats = {s["src"]: s for s in ha.station_stats([
            packet(rssi=-110.0, snr=-6.0, ferr=1000, host_utc="2026-11-09T12:00:00.000Z"),
            packet(rssi=-100.0, snr=-2.0, ferr=1200, host_utc="2026-11-09T12:10:00.000Z", path="SM0XYZ-1*"),
            packet(crc_ok=0, rssi=-120.0, snr=-18.0, ferr=900, host_utc="2026-11-09T12:20:00.000Z"),
            packet(src="N0CALL-9", distance=30.0),
            packet(src=None),
        ])}

    def test_packets_without_a_source_are_left_out(self):
        self.assertEqual(sorted(self.stats), ["N0CALL-7", "N0CALL-9"])

    def test_packet_count(self):
        self.assertEqual(self.stats["N0CALL-7"]["packets"], 3)

    def test_crc_failures(self):
        self.assertEqual(self.stats["N0CALL-7"]["crc_fail"], 1)

    def test_rssi_range_uses_good_packets_only(self):
        s = self.stats["N0CALL-7"]
        self.assertEqual((s["rssi_min"], s["rssi_max"]), (-110.0, -100.0))

    def test_mean_link_margin(self):
        self.assertEqual(self.stats["N0CALL-7"]["margin_mean"], 16.0)

    def test_frequency_error_mean(self):
        self.assertEqual(self.stats["N0CALL-7"]["ferr_mean"], 1100.0)

    def test_frequency_error_spread(self):
        self.assertAlmostEqual(self.stats["N0CALL-7"]["ferr_sd"], 141.42, places=2)

    def test_direct_and_digipeated_counts(self):
        s = self.stats["N0CALL-7"]
        self.assertEqual((s["direct"], s["via"]), (1, {"SM0XYZ-1": 1}))

    def test_last_heard(self):
        self.assertEqual(self.stats["N0CALL-7"]["last_utc"], "2026-11-09T12:10:00.000Z")


class HourlyTests(unittest.TestCase):
    def test_packets_are_counted_per_hour(self):
        hours = ha.packets_per_hour([
            packet(host_utc="2026-11-09T12:00:00.000Z"),
            packet(host_utc="2026-11-09T12:59:00.000Z", crc_ok=0),
            packet(host_utc="2026-11-09T13:05:00.000Z"),
        ])
        self.assertEqual(hours, [
            {"hour": "2026-11-09T12", "ok": 1, "fail": 1},
            {"hour": "2026-11-09T13", "ok": 1, "fail": 0},
        ])


class HexdumpTests(unittest.TestCase):
    def test_one_line(self):
        self.assertEqual(ha.hexdump("3cff014e30"), ["0000  3c ff 01 4e 30" + " " * 33 + "  <..N0"])

    def test_sixteen_bytes_per_line(self):
        self.assertEqual(len(ha.hexdump("00" * 17)), 2)


if __name__ == "__main__":
    unittest.main()
