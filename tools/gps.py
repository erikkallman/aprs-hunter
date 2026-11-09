"""Receiver position and GPS time for tagging records.

Sources:
  - a fixed position (the logger's --home),
  - an NMEA GPS on a serial port (the logger's --gps-port), read by
    GpsSerialReader in a background thread.

PositionTracker holds the latest fix and answers two questions for each
record: where was the receiver, and what was the GPS time.
"""

import datetime
import threading
import time
from dataclasses import dataclass
from typing import Callable, Optional, Tuple

UTC = datetime.timezone.utc


@dataclass(frozen=True)
class Fix:
    latitude: float
    longitude: float
    altitude_m: Optional[float] = None
    utc: Optional[datetime.datetime] = None  # GPS time of the fix
    satellites: Optional[int] = None
    hdop: Optional[float] = None
    source: str = "gps"  # "gps" or "fixed"


def format_utc(moment: datetime.datetime) -> str:
    """ISO 8601 UTC with milliseconds, for example 2026-11-09T10:15:30.000Z."""
    return moment.astimezone(UTC).isoformat(timespec="milliseconds").replace("+00:00", "Z")


# ---- NMEA ----

def _checksum_ok(sentence: str) -> bool:
    if not sentence.startswith("$") or "*" not in sentence:
        return False
    body, _, given = sentence[1:].partition("*")
    calculated = 0
    for ch in body:
        calculated ^= ord(ch)
    try:
        return calculated == int(given[:2], 16)
    except ValueError:
        return False


def _degrees(value: str, hemisphere: str, degree_digits: int) -> Optional[float]:
    """Convert NMEA ddmm.mmmm (or dddmm.mmmm) to signed decimal degrees."""
    if not value or not hemisphere:
        return None
    degrees = int(value[:degree_digits]) + float(value[degree_digits:]) / 60.0
    return -degrees if hemisphere in ("S", "W") else degrees


def _time_of_day(value: str) -> Optional[datetime.time]:
    """NMEA hhmmss or hhmmss.ss."""
    if len(value) < 6:
        return None
    seconds = float(value[4:])
    whole = int(seconds)
    return datetime.time(int(value[0:2]), int(value[2:4]), whole,
                         int(round((seconds - whole) * 1_000_000)), tzinfo=UTC)


def _optional_float(value: str) -> Optional[float]:
    return float(value) if value else None


def parse_nmea(sentence: str) -> Optional[dict]:
    """Parse an RMC or GGA sentence from any talker (GP, GN, GL, ...).

    Returns None for other sentences, bad checksums, and malformed text.
    """
    sentence = sentence.strip()
    if not _checksum_ok(sentence):
        return None
    fields = sentence[1:].split("*")[0].split(",")
    kind = fields[0][2:]
    try:
        if kind == "RMC" and len(fields) >= 10:
            time_of_day = _time_of_day(fields[1])
            date = fields[9]
            utc = None
            if time_of_day and len(date) == 6:
                year = int(date[4:6])
                year += 2000 if year < 80 else 1900
                utc = datetime.datetime.combine(
                    datetime.date(year, int(date[2:4]), int(date[0:2])), time_of_day)
            return {
                "type": "RMC",
                "valid": fields[2] == "A",
                "utc": utc,
                "latitude": _degrees(fields[3], fields[4], 2),
                "longitude": _degrees(fields[5], fields[6], 3),
            }
        if kind == "GGA" and len(fields) >= 10:
            return {
                "type": "GGA",
                "time": _time_of_day(fields[1]),
                "latitude": _degrees(fields[2], fields[3], 2),
                "longitude": _degrees(fields[4], fields[5], 3),
                "quality": int(fields[6]) if fields[6] else 0,
                "satellites": int(fields[7]) if fields[7] else None,
                "hdop": _optional_float(fields[8]),
                "altitude_m": _optional_float(fields[9]),
            }
    except ValueError:
        return None
    return None


class FixBuilder:
    """Combine RMC (time, date, position) with the GGA of the same second
    (altitude, satellites, HDOP) into a Fix."""

    def __init__(self):
        self.last_gga = None

    def feed(self, sentence: str) -> Optional[Fix]:
        """Feed one line. Returns a Fix when a valid RMC arrives."""
        parsed = parse_nmea(sentence)
        if parsed is None:
            return None
        if parsed["type"] == "GGA":
            self.last_gga = parsed
            return None
        if not parsed["valid"] or parsed["latitude"] is None or parsed["longitude"] is None:
            return None

        gga = self.last_gga
        same_second = (gga is not None and parsed["utc"] is not None and gga["time"] is not None
                       and gga["time"].replace(tzinfo=None) == parsed["utc"].time())
        return Fix(
            latitude=parsed["latitude"],
            longitude=parsed["longitude"],
            altitude_m=gga["altitude_m"] if same_second else None,
            utc=parsed["utc"],
            satellites=gga["satellites"] if same_second else None,
            hdop=gga["hdop"] if same_second else None,
            source="gps",
        )


# ---- Tracking ----

class PositionTracker:
    """The receiver's latest position and GPS time. Safe to update from the
    GPS reader thread while the logger reads it."""

    def __init__(self, fixed: Optional[Tuple[float, float]] = None,
                 position_max_age_s: float = 10.0, time_max_age_s: float = 3600.0,
                 clock: Callable[[], float] = time.monotonic):
        self.fixed = Fix(latitude=fixed[0], longitude=fixed[1], source="fixed") if fixed else None
        self.position_max_age_s = position_max_age_s
        self.time_max_age_s = time_max_age_s
        self.clock = clock
        self.lock = threading.Lock()
        self.latest = None
        self.latest_at = 0.0

    def update(self, fix: Fix) -> None:
        with self.lock:
            self.latest = fix
            self.latest_at = self.clock()

    def _gps(self, max_age_s: float) -> Optional[Tuple[Fix, float]]:
        with self.lock:
            if self.latest is None:
                return None
            age = self.clock() - self.latest_at
            return (self.latest, age) if age <= max_age_s else None

    def current(self) -> Optional[Fix]:
        """A fresh GPS fix, else the fixed position, else None."""
        recent = self._gps(self.position_max_age_s)
        return recent[0] if recent else self.fixed

    def gps_utc(self) -> Optional[datetime.datetime]:
        """GPS time now: the last fix time plus the time passed since it.
        Lasts longer than the position, because the clock keeps time well
        after the receiver loses its fix (for example indoors)."""
        recent = self._gps(self.time_max_age_s)
        if not recent or recent[0].utc is None:
            return None
        fix, age = recent
        return fix.utc + datetime.timedelta(seconds=age)

    def tag(self) -> dict:
        """The receiver fields stored with each record."""
        position = self.current()
        gps_time = self.gps_utc()
        receiver = None
        if position is not None:
            receiver = {
                "lat": round(position.latitude, 7),
                "lon": round(position.longitude, 7),
                "alt_m": position.altitude_m,
                "source": position.source,
                "fix_utc": format_utc(position.utc) if position.utc else None,
                "sats": position.satellites,
                "hdop": position.hdop,
            }
        return {"receiver": receiver, "gps_utc": format_utc(gps_time) if gps_time else None}


class GpsSerialReader(threading.Thread):
    """Read an NMEA GPS from a serial port and feed the tracker. Reconnects
    every few seconds if the port goes away."""

    RETRY_S = 5.0

    def __init__(self, port: str, baud: int, tracker: PositionTracker, report: Callable[[str], None] = print):
        super().__init__(name="gps-reader", daemon=True)
        self.port = port
        self.baud = baud
        self.tracker = tracker
        self.report = report

    def run(self) -> None:
        import serial  # only needed when a GPS is attached

        builder = FixBuilder()
        while True:
            try:
                with serial.Serial(self.port, self.baud, timeout=2) as gps_port:
                    self.report("GPS: reading {} at {} baud".format(self.port, self.baud))
                    while True:
                        line = gps_port.readline().decode("ascii", errors="replace")
                        fix = builder.feed(line)
                        if fix is not None:
                            self.tracker.update(fix)
            except (OSError, serial.SerialException) as error:
                self.report("GPS: {} ({}); retrying in {:.0f} s".format(self.port, error, self.RETRY_S))
                time.sleep(self.RETRY_S)
