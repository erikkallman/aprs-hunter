"""Tests for gps. Run with: make test-tools"""

import datetime
import unittest

import gps

# Classic NMEA examples (checksums verified).
RMC = "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A"
GGA = "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47"
# A multi-system receiver (GN talker) near Stockholm.
GN_RMC = "$GNRMC,101530.00,A,5919.8000,N,01803.6000,E,0.05,,091126,,,A*50"
GN_GGA = "$GNGGA,101530.00,5919.8000,N,01803.6000,E,1,09,0.95,28.4,M,24.1,M,,*78"
VOID_RMC = "$GNRMC,101531.00,V,,,,,,,091126,,,N*69"

UTC = datetime.timezone.utc


class FakeClock:
    """A monotonic clock the test can move."""

    def __init__(self, now=1000.0):
        self.now = now

    def __call__(self):
        return self.now


class ParseNmeaTests(unittest.TestCase):
    def test_bad_checksum_is_rejected(self):
        self.assertIsNone(gps.parse_nmea(RMC[:-2] + "00"))

    def test_other_sentences_are_ignored(self):
        self.assertIsNone(gps.parse_nmea("$GPGSV,3,1,11,03,03,111,00,04,15,270,00,06,01,010,00,13,06,292,00*74"))

    def test_rmc_latitude(self):
        self.assertAlmostEqual(gps.parse_nmea(RMC)["latitude"], 48.1173, places=4)

    def test_rmc_longitude(self):
        self.assertAlmostEqual(gps.parse_nmea(RMC)["longitude"], 11.516667, places=5)

    def test_rmc_time_and_date(self):
        self.assertEqual(gps.parse_nmea(RMC)["utc"], datetime.datetime(1994, 3, 23, 12, 35, 19, tzinfo=UTC))

    def test_rmc_fractional_seconds(self):
        self.assertEqual(gps.parse_nmea(GN_RMC)["utc"], datetime.datetime(2026, 11, 9, 10, 15, 30, tzinfo=UTC))

    def test_rmc_status_valid(self):
        self.assertTrue(gps.parse_nmea(RMC)["valid"])

    def test_void_rmc_is_not_valid(self):
        self.assertFalse(gps.parse_nmea(VOID_RMC)["valid"])

    def test_gga_altitude(self):
        self.assertEqual(gps.parse_nmea(GGA)["altitude_m"], 545.4)

    def test_gga_satellites(self):
        self.assertEqual(gps.parse_nmea(GGA)["satellites"], 8)

    def test_gga_hdop(self):
        self.assertEqual(gps.parse_nmea(GGA)["hdop"], 0.9)

    def test_southern_and_western_hemispheres_are_negative(self):
        body = "GPRMC,000000,A,3352.000,S,15112.000,W,0.0,0.0,010126,,"
        checksum = 0
        for ch in body:
            checksum ^= ord(ch)
        parsed = gps.parse_nmea("${}*{:02X}".format(body, checksum))
        self.assertEqual((round(parsed["latitude"], 4), round(parsed["longitude"], 4)), (-33.8667, -151.2))


class FixBuilderTests(unittest.TestCase):
    def test_valid_rmc_gives_a_fix(self):
        self.assertIsNotNone(gps.FixBuilder().feed(GN_RMC))

    def test_void_rmc_gives_no_fix(self):
        self.assertIsNone(gps.FixBuilder().feed(VOID_RMC))

    def test_gga_alone_gives_no_fix(self):
        self.assertIsNone(gps.FixBuilder().feed(GN_GGA))

    def test_fix_takes_altitude_from_the_matching_gga(self):
        builder = gps.FixBuilder()
        builder.feed(GN_GGA)
        self.assertEqual(builder.feed(GN_RMC).altitude_m, 28.4)

    def test_fix_takes_satellites_from_the_matching_gga(self):
        builder = gps.FixBuilder()
        builder.feed(GN_GGA)
        self.assertEqual(builder.feed(GN_RMC).satellites, 9)

    def test_gga_from_another_second_is_not_used(self):
        builder = gps.FixBuilder()
        builder.feed(GGA)  # 12:35:19, not 10:15:30
        self.assertIsNone(builder.feed(GN_RMC).altitude_m)


class PositionTrackerTests(unittest.TestCase):
    def setUp(self):
        self.clock = FakeClock()
        self.fix = gps.FixBuilder().feed(GN_RMC)

    def test_no_sources_gives_no_position(self):
        self.assertIsNone(gps.PositionTracker(clock=self.clock).current())

    def test_fixed_position_is_used_without_gps(self):
        tracker = gps.PositionTracker(fixed=(59.0, 18.0), clock=self.clock)
        self.assertEqual(tracker.current().source, "fixed")

    def test_fresh_gps_fix_wins_over_the_fixed_position(self):
        tracker = gps.PositionTracker(fixed=(59.0, 18.0), clock=self.clock)
        tracker.update(self.fix)
        self.assertEqual(tracker.current().source, "gps")

    def test_stale_gps_fix_falls_back_to_the_fixed_position(self):
        tracker = gps.PositionTracker(fixed=(59.0, 18.0), position_max_age_s=10, clock=self.clock)
        tracker.update(self.fix)
        self.clock.now += 11
        self.assertEqual(tracker.current().source, "fixed")

    def test_gps_time_adds_the_time_since_the_fix(self):
        tracker = gps.PositionTracker(clock=self.clock)
        tracker.update(self.fix)
        self.clock.now += 2.5
        self.assertEqual(tracker.gps_utc(), datetime.datetime(2026, 11, 9, 10, 15, 32, 500000, tzinfo=UTC))

    def test_gps_time_outlives_the_position(self):
        tracker = gps.PositionTracker(position_max_age_s=10, time_max_age_s=3600, clock=self.clock)
        tracker.update(self.fix)
        self.clock.now += 600
        self.assertIsNotNone(tracker.gps_utc())

    def test_gps_time_expires(self):
        tracker = gps.PositionTracker(time_max_age_s=3600, clock=self.clock)
        tracker.update(self.fix)
        self.clock.now += 3601
        self.assertIsNone(tracker.gps_utc())

    def test_tag_without_sources(self):
        self.assertEqual(gps.PositionTracker(clock=self.clock).tag(), {"receiver": None, "gps_utc": None})

    def test_tag_with_a_gps_fix(self):
        tracker = gps.PositionTracker(clock=self.clock)
        tracker.update(self.fix)
        self.assertEqual(tracker.tag(), {
            "receiver": {"lat": 59.33, "lon": 18.06, "alt_m": None, "source": "gps",
                         "fix_utc": "2026-11-09T10:15:30.000Z", "sats": None, "hdop": None},
            "gps_utc": "2026-11-09T10:15:30.000Z",
        })


if __name__ == "__main__":
    unittest.main()
