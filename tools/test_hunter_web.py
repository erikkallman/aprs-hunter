"""Tests for hunter_web: a real server on a free port over a temporary
database filled through HunterLog. Run with: make test-tools"""

import json
import pathlib
import tempfile
import unittest
import urllib.error
import urllib.request

import gps
import hunter_log as hl
import hunter_web as hw
import test_hunter_log as fixtures

GLITCH_NOISE = json.dumps({
    "v": 1, "type": "noise", "uptime_ms": 20000, "freq_hz": 433775000, "samples": 93,
    "min_dbm": -164.0, "mean_dbm": -101.0, "max_dbm": -37.0,
})

CHECKED_NOISE = json.dumps({
    "v": 1, "type": "noise", "uptime_ms": 40000, "freq_hz": 433775000, "samples": 93,
    "read_mismatches": 5, "skipped_samples": 2, "unconfirmed_samples": 4,
    "min_dbm": -108.0, "mean_dbm": -101.0, "max_dbm": -99.0,
})


class WebServerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        db_path = pathlib.Path(cls.tmp.name) / "hunter.sqlite"
        conn = hl.open_db(db_path)
        log = hl.HunterLog(conn, positions=gps.PositionTracker(fixed=(59.0, 18.0)))
        lines = [
            (fixtures.BOOT_LINE, "2026-11-09T12:00:00.000Z"),
            (fixtures.NOISE_LINE, "2026-11-09T12:00:10.000Z"),
            (fixtures.rx_line(seq=1), "2026-11-09T12:00:12.000Z"),
            (GLITCH_NOISE, "2026-11-09T12:00:20.000Z"),
            (fixtures.rx_line(text=fixtures.WEATHER, seq=2), "2026-11-09T12:00:25.000Z"),
            (CHECKED_NOISE, "2026-11-09T12:00:40.000Z"),
            (fixtures.rx_line(seq=3, crc_ok=False), "2026-11-09T13:00:00.000Z"),
            (fixtures.ERROR_LINE, "2026-11-09T13:00:05.000Z"),
        ]
        for line, host_utc in lines:
            log.handle_line(line, host_utc)
        conn.commit()
        conn.close()

        cls.server = hw.make_server(db_path, "127.0.0.1", 0)
        hw.start_in_background(cls.server)
        cls.base = "http://127.0.0.1:{}".format(cls.server.server_address[1])

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.tmp.cleanup()

    def get(self, path):
        with urllib.request.urlopen(self.base + path, timeout=5) as response:
            return response.status, response.read()

    def get_json(self, path):
        status, body = self.get(path)
        return json.loads(body)

    def status_of(self, path):
        try:
            return self.get(path)[0]
        except urllib.error.HTTPError as error:
            return error.code

    def test_page_is_served(self):
        status, body = self.get("/")
        self.assertEqual((status, b"<title>APRS hunter</title>" in body), (200, True))

    def test_script_is_served(self):
        self.assertEqual(self.status_of("/app.js"), 200)

    def test_files_outside_the_page_are_not_served(self):
        self.assertEqual(self.status_of("/../hunter_log.py"), 404)

    def test_unknown_api_route(self):
        self.assertEqual(self.status_of("/api/nothing"), 404)

    def test_bad_packet_id(self):
        self.assertEqual(self.status_of("/api/packet?id=abc"), 400)

    def test_missing_packet(self):
        self.assertEqual(self.status_of("/api/packet?id=999"), 404)

    def test_summary_counts_packets(self):
        summary = self.get_json("/api/summary")
        self.assertEqual((summary["packets"], summary["ok"], summary["crc_fail"]), (3, 2, 1))

    def test_summary_counts_stations_with_good_packets(self):
        self.assertEqual(self.get_json("/api/summary")["stations"], 2)

    def test_summary_counts_noise_glitches(self):
        self.assertEqual(self.get_json("/api/summary")["noise_glitches"], 1)

    def test_summary_sums_noise_read_mismatches(self):
        self.assertEqual(self.get_json("/api/summary")["noise_read_mismatches"], 5)

    def test_summary_sums_noise_skipped_samples(self):
        self.assertEqual(self.get_json("/api/summary")["noise_skipped_samples"], 2)

    def test_summary_sums_noise_unconfirmed_samples(self):
        self.assertEqual(self.get_json("/api/summary")["noise_unconfirmed_samples"], 4)

    def test_summary_has_no_read_mismatches_without_checked_noise(self):
        self.assertIsNone(self.get_json("/api/summary?since=2026-11-09T12:00:41.000Z")["noise_read_mismatches"])

    def test_summary_reports_the_receiver_source(self):
        self.assertEqual(self.get_json("/api/summary")["receiver"]["rx_source"], "fixed")

    def test_summary_honours_the_time_range(self):
        self.assertEqual(self.get_json("/api/summary?since=2026-11-09T12:30:00.000Z")["packets"], 1)

    def test_packets_come_newest_first(self):
        self.assertEqual([p["seq"] for p in self.get_json("/api/packets")], [3, 2, 1])

    def test_packets_carry_the_link_margin(self):
        # SNR -7.25 dB at SF12 (floor -20 dB)
        self.assertEqual(self.get_json("/api/packets")[0]["link_margin_db"], 12.75)

    def test_packets_carry_decoded_irq_flags(self):
        self.assertEqual(self.get_json("/api/packets")[0]["irq_flags"], ["RxDone", "ValidHeader"])

    def test_packet_detail_has_a_hexdump(self):
        self.assertTrue(self.get_json("/api/packet?id=1")["hexdump"][0].startswith("0000  3c ff 01"))

    def test_packet_detail_has_the_nearest_noise(self):
        self.assertEqual(self.get_json("/api/packet?id=1")["noise_near"]["seconds_from_packet"], -2.0)

    def test_packet_detail_has_the_boot(self):
        self.assertEqual(self.get_json("/api/packet?id=1")["boot"]["fw"], "0.1.0")

    def test_stations_count_crc_failures(self):
        stations = {s["src"]: s for s in self.get_json("/api/stations")}
        self.assertEqual(stations["N0CALL-7"]["crc_fail"], 1)

    def test_noise_points_flag_glitches(self):
        self.assertEqual([p["glitches"] for p in self.get_json("/api/noise")], [0, 1, 0])

    def test_noise_points_carry_unconfirmed_samples(self):
        self.assertEqual([p["unconfirmed_samples"] for p in self.get_json("/api/noise")], [None, None, 4])

    def test_noise_points_carry_read_mismatches(self):
        self.assertEqual([p["read_mismatches"] for p in self.get_json("/api/noise")], [None, None, 5])

    def test_hourly_counts(self):
        self.assertEqual(self.get_json("/api/hourly"), [
            {"hour": "2026-11-09T12", "ok": 2, "fail": 0},
            {"hour": "2026-11-09T13", "ok": 0, "fail": 1},
        ])

    def test_events_list_boots_and_errors(self):
        events = self.get_json("/api/events")
        self.assertEqual((len(events["boots"]), len(events["errors"])), (1, 1))


if __name__ == "__main__":
    unittest.main()
