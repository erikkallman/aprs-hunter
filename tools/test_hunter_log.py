"""Tests for hunter_log. Run with: make test-tools"""

import argparse
import contextlib
import io
import json
import pathlib
import sqlite3
import tempfile
import unittest

import gps
import hunter_log as hl

HOST_UTC = "2026-11-09T12:00:00.000Z"
TRACKER = "N0CALL-7>APLRT1,WIDE1-1:!5920.00N/01804.00E>090/036/A=000328 Test tracker"
WEATHER = "N0CALL-13>APLRW1:_10090556c220s004g005t077r000p000P000h50b09900wRSW"


def frame_hex(text, prefix=True):
    return ("3cff01" if prefix else "") + text.encode("latin-1").hex()


def rx_line(text=TRACKER, crc_ok=True, seq=1):
    return json.dumps({
        "v": 1, "type": "rx", "seq": seq, "uptime_ms": 5000, "freq_hz": 433775000,
        "rssi_dbm": -112.5, "snr_db": -7.25, "freq_err_hz": -1234, "crc_ok": crc_ok,
        "rx_cr": 5, "irq": 80, "modem_stat": 36, "toa_us": 1155072,
        "len": len(text) + 3, "raw_hex": frame_hex(text),
    })


BOOT_LINE = json.dumps({
    "v": 1, "type": "boot", "fw": "0.1.0", "uptime_ms": 12, "reset_reason": 1, "radio_status": 0,
    "radio": {"freq_hz": 433775000, "bw_hz": 125000, "sf": 12, "cr": 5, "sync_word": 18, "preamble": 8},
})
NOISE_LINE = json.dumps({
    "v": 1, "type": "noise", "uptime_ms": 10000, "freq_hz": 433775000, "samples": 100,
    "min_dbm": -120.0, "mean_dbm": -117.5, "max_dbm": -95.0,
})
# Firmware 0.3.0 and later also count RSSI reads it dropped.
CHECKED_NOISE_LINE = json.dumps({
    "v": 1, "type": "noise", "uptime_ms": 20000, "freq_hz": 433775000, "samples": 93,
    "read_mismatches": 3, "skipped_samples": 1,
    "min_dbm": -108.0, "mean_dbm": -101.2, "max_dbm": -99.0,
})
ERROR_LINE = json.dumps({"v": 1, "type": "error", "uptime_ms": 5000, "where": "radio.readData", "code": -2})
# Firmware 0.3.2 and later log the raw RSSI reads around a jump.
TRACE_LINE = json.dumps({
    "v": 1, "type": "rssi_trace", "uptime_ms": 5000, "ref_dbm": -101.3, "ok": True,
    "value_dbm": -69.0, "stable_reads": 2, "missed": 0,
    "reads_dbm": [-101, -69, -69], "t_us": [0, 25, 51]})


class ParseLineTests(unittest.TestCase):
    def test_boot_rom_text_is_skipped(self):
        self.assertIsNone(hl.parse_line("rst:0x1 (POWERON_RESET),boot:0x13 (SPI_FAST_FLASH_BOOT)"))

    def test_broken_json_is_skipped(self):
        self.assertIsNone(hl.parse_line('{"v":1,"type":"rx"'))

    def test_json_without_a_type_is_skipped(self):
        self.assertIsNone(hl.parse_line('{"v":1}'))

    def test_record_is_returned(self):
        self.assertEqual(hl.parse_line(NOISE_LINE + "\r\n")["type"], "noise")


class DecodeAprsTests(unittest.TestCase):
    def test_position_latitude(self):
        self.assertAlmostEqual(hl.decode_aprs(frame_hex(TRACKER))["latitude"], 59.3333, places=4)

    def test_source(self):
        self.assertEqual(hl.decode_aprs(frame_hex(TRACKER))["src"], "N0CALL-7")

    def test_path(self):
        self.assertEqual(hl.decode_aprs(frame_hex(TRACKER))["path"], "WIDE1-1")

    def test_info_is_the_text_after_the_header(self):
        self.assertEqual(hl.decode_aprs(frame_hex(TRACKER))["info"], "!5920.00N/01804.00E>090/036/A=000328 Test tracker")

    def test_weather_temperature(self):
        self.assertEqual(hl.decode_aprs(frame_hex(WEATHER))["weather"]["temperature"], 25.0)

    def test_missing_prefix_is_reported(self):
        self.assertEqual(hl.decode_aprs(frame_hex(TRACKER, prefix=False))["decode_error"], "no LoRa APRS prefix")

    def test_parse_error_is_reported(self):
        self.assertTrue(hl.decode_aprs(frame_hex("garbage without header"))["decode_error"].startswith("ParseError"))


class DistanceBearingTests(unittest.TestCase):
    def test_one_degree_north_is_about_111_km(self):
        distance, _ = hl.distance_bearing(59.0, 18.0, 60.0, 18.0)
        self.assertAlmostEqual(distance, 111.19, places=1)

    def test_bearing_north(self):
        _, bearing = hl.distance_bearing(59.0, 18.0, 60.0, 18.0)
        self.assertAlmostEqual(bearing, 0.0, places=3)

    def test_bearing_east(self):
        _, bearing = hl.distance_bearing(0.0, 0.0, 0.0, 1.0)
        self.assertAlmostEqual(bearing, 90.0, places=3)

    def test_parse_home(self):
        self.assertEqual(hl.parse_home("59.0, 18.5"), (59.0, 18.5))

    def test_parse_home_rejects_text(self):
        with self.assertRaisesRegex(argparse.ArgumentTypeError, "for example 59.33,18.06"):
            hl.parse_home("LAT,LON")

    def test_parse_home_rejects_one_number(self):
        with self.assertRaises(argparse.ArgumentTypeError):
            hl.parse_home("59.33")

    def test_parse_home_rejects_latitude_out_of_range(self):
        with self.assertRaisesRegex(argparse.ArgumentTypeError, "latitude"):
            hl.parse_home("95.0,18.0")

    def test_parse_home_rejects_longitude_out_of_range(self):
        with self.assertRaisesRegex(argparse.ArgumentTypeError, "longitude"):
            hl.parse_home("59.0,190.0")

    def test_bad_home_stops_with_a_usage_error(self):
        # argparse exits with status 2 and prints the message instead of a traceback.
        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit) as stop:
                hl.main(["--replay", "unused.jsonl", "--home", "LAT,LON"])
        self.assertEqual(stop.exception.code, 2)


class HunterLogTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.archive_dir = pathlib.Path(self.tmp.name)
        self.conn = hl.open_db(":memory:")
        self.log = hl.HunterLog(self.conn, archive_dir=self.archive_dir,
                                positions=gps.PositionTracker(fixed=(59.0, 18.0)))

    def tearDown(self):
        self.conn.close()
        self.tmp.cleanup()

    def scalar(self, sql):
        return self.conn.execute(sql).fetchone()[0]

    def test_packet_is_stored(self):
        self.log.handle_line(rx_line(), HOST_UTC)
        self.assertEqual(self.scalar("SELECT count(*) FROM packets"), 1)

    def test_packet_source_is_stored(self):
        self.log.handle_line(rx_line(), HOST_UTC)
        self.assertEqual(self.scalar("SELECT src FROM packets"), "N0CALL-7")

    def test_packet_distance_from_home(self):
        self.log.handle_line(rx_line(), HOST_UTC)
        self.assertAlmostEqual(self.scalar("SELECT distance_km FROM packets"), 37.26, places=1)

    def test_frequency_error_in_ppm(self):
        self.log.handle_line(rx_line(), HOST_UTC)
        self.assertAlmostEqual(self.scalar("SELECT freq_err_ppm FROM packets"), -2.845, places=3)

    def test_crc_failure_is_stored(self):
        self.log.handle_line(rx_line(crc_ok=False), HOST_UTC)
        self.assertEqual(self.scalar("SELECT crc_ok FROM packets"), 0)

    def test_noise_is_stored(self):
        self.log.handle_line(NOISE_LINE, HOST_UTC)
        self.assertEqual(self.scalar("SELECT max_dbm FROM noise"), -95.0)

    def test_noise_read_mismatches_are_stored(self):
        self.log.handle_line(CHECKED_NOISE_LINE, HOST_UTC)
        self.assertEqual(self.scalar("SELECT read_mismatches FROM noise"), 3)

    def test_noise_skipped_samples_are_stored(self):
        self.log.handle_line(CHECKED_NOISE_LINE, HOST_UTC)
        self.assertEqual(self.scalar("SELECT skipped_samples FROM noise"), 1)

    def test_noise_from_older_firmware_has_no_read_mismatches(self):
        self.log.handle_line(NOISE_LINE, HOST_UTC)
        self.assertIsNone(self.scalar("SELECT read_mismatches FROM noise"))

    def test_error_is_stored(self):
        self.log.handle_line(ERROR_LINE, HOST_UTC)
        self.assertEqual(self.scalar("SELECT step FROM errors"), "radio.readData")

    def test_boot_links_following_packets(self):
        self.log.handle_line(BOOT_LINE, HOST_UTC)
        self.log.handle_line(rx_line(), HOST_UTC)
        self.assertEqual(self.scalar("SELECT boot_id FROM packets"), self.scalar("SELECT id FROM boots"))

    def test_archive_line_has_host_time(self):
        self.log.handle_line(rx_line(), HOST_UTC)
        line = (self.archive_dir / "2026-11-09.jsonl").read_text().splitlines()[0]
        self.assertEqual(json.loads(line)["host_utc"], HOST_UTC)

    def test_replayed_line_keeps_its_host_time(self):
        replay = hl.HunterLog(self.conn, archive_dir=None)
        archived = json.dumps({"host_utc": "2026-11-01T08:00:00.000Z", **json.loads(rx_line())})
        replay.handle_line(archived, HOST_UTC)
        self.assertEqual(self.scalar("SELECT host_utc FROM packets"), "2026-11-01T08:00:00.000Z")

    def test_packet_summary_names_the_source(self):
        self.assertIn("N0CALL-7", self.log.handle_line(rx_line(), HOST_UTC))

    def test_trace_is_archived(self):
        self.log.handle_line(TRACE_LINE, HOST_UTC)
        line = (self.archive_dir / "2026-11-09.jsonl").read_text().splitlines()[0]
        self.assertEqual(json.loads(line)["reads_dbm"], [-101, -69, -69])

    def test_trace_is_not_stored_as_noise(self):
        self.log.handle_line(TRACE_LINE, HOST_UTC)
        self.assertEqual(self.scalar("SELECT count(*) FROM noise"), 0)

    def test_trace_has_no_summary(self):
        self.assertIsNone(self.log.handle_line(TRACE_LINE, HOST_UTC))

    def test_noise_has_no_summary(self):
        self.assertIsNone(self.log.handle_line(NOISE_LINE, HOST_UTC))


class FakeClock:
    def __init__(self, now=1000.0):
        self.now = now

    def __call__(self):
        return self.now


# A GPS fix near Stockholm at 2026-11-09 10:15:30 UTC.
GPS_RMC = "$GNRMC,101530.00,A,5919.8000,N,01803.6000,E,0.05,,091126,,,A*50"


class ReceiverTaggingTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.archive_dir = pathlib.Path(self.tmp.name)
        self.conn = hl.open_db(":memory:")
        self.clock = FakeClock()
        self.positions = gps.PositionTracker(fixed=(59.0, 18.0), clock=self.clock)
        self.log = hl.HunterLog(self.conn, archive_dir=self.archive_dir, positions=self.positions)

    def tearDown(self):
        self.conn.close()
        self.tmp.cleanup()

    def scalar(self, sql):
        return self.conn.execute(sql).fetchone()[0]

    def use_gps(self):
        self.positions.update(gps.FixBuilder().feed(GPS_RMC))

    def test_packet_without_gps_uses_the_fixed_position(self):
        self.log.handle_line(rx_line(), HOST_UTC)
        self.assertEqual(self.scalar("SELECT rx_source FROM packets"), "fixed")

    def test_packet_with_gps_stores_the_gps_position(self):
        self.use_gps()
        self.log.handle_line(rx_line(), HOST_UTC)
        self.assertAlmostEqual(self.scalar("SELECT rx_lat FROM packets"), 59.33, places=5)

    def test_packet_with_gps_stores_gps_time(self):
        self.use_gps()
        self.clock.now += 1.25
        self.log.handle_line(rx_line(), HOST_UTC)
        self.assertEqual(self.scalar("SELECT gps_utc FROM packets"), "2026-11-09T10:15:31.250Z")

    def test_packet_without_gps_has_no_gps_time(self):
        self.log.handle_line(rx_line(), HOST_UTC)
        self.assertIsNone(self.scalar("SELECT gps_utc FROM packets"))

    def test_distance_is_measured_from_the_gps_position(self):
        # The tracker in the packet sits about 0.5 km from the GPS fix.
        self.use_gps()
        self.log.handle_line(rx_line(), HOST_UTC)
        self.assertLess(self.scalar("SELECT distance_km FROM packets"), 1.0)

    def test_noise_is_tagged_with_the_receiver_position(self):
        self.use_gps()
        self.log.handle_line(NOISE_LINE, HOST_UTC)
        self.assertEqual(self.scalar("SELECT rx_source FROM noise"), "gps")

    def test_archive_line_carries_the_receiver_position(self):
        self.use_gps()
        self.log.handle_line(rx_line(), HOST_UTC)
        line = (self.archive_dir / "2026-11-09.jsonl").read_text().splitlines()[0]
        self.assertEqual(json.loads(line)["receiver"]["source"], "gps")

    def test_replay_uses_the_archived_receiver_position(self):
        archived = json.dumps({
            "host_utc": HOST_UTC, "gps_utc": "2026-11-09T10:15:31.250Z",
            "receiver": {"lat": 60.0, "lon": 17.0, "alt_m": 12.0, "source": "gps",
                         "fix_utc": "2026-11-09T10:15:30.000Z", "sats": 7, "hdop": 1.1},
            **json.loads(rx_line()),
        })
        hl.HunterLog(self.conn, archive_dir=None).handle_line(archived, HOST_UTC)
        self.assertEqual(self.scalar("SELECT rx_lat FROM packets"), 60.0)


class MigrationTests(unittest.TestCase):
    def test_old_database_gains_the_receiver_columns(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / "old.sqlite"
            old = sqlite3.connect(str(path))
            old.execute("CREATE TABLE packets (id INTEGER PRIMARY KEY, host_utc TEXT, src TEXT)")
            old.execute("INSERT INTO packets (host_utc, src) VALUES ('2026-11-01T00:00:00.000Z', 'N0CALL')")
            old.commit()
            old.close()

            conn = hl.open_db(path)
            columns = {row[1] for row in conn.execute("PRAGMA table_info(packets)")}
            rows = conn.execute("SELECT count(*) FROM packets").fetchone()[0]
            conn.close()
        self.assertEqual((("rx_lat" in columns), ("gps_utc" in columns), rows), (True, True, 1))

    def test_old_noise_table_gains_the_read_check_columns(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / "old.sqlite"
            old = sqlite3.connect(str(path))
            old.execute("CREATE TABLE noise (id INTEGER PRIMARY KEY, host_utc TEXT, min_dbm REAL)")
            old.commit()
            old.close()

            conn = hl.open_db(path)
            columns = {row[1] for row in conn.execute("PRAGMA table_info(noise)")}
            conn.close()
        self.assertEqual((("read_mismatches" in columns), ("skipped_samples" in columns)), (True, True))


if __name__ == "__main__":
    unittest.main()
