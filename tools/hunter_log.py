"""Log APRS hunter records from the board's USB serial port.

The board prints one JSON record per line. This script:
  - adds the computer's UTC time to each record,
  - appends every record to a daily JSON Lines archive (<data-dir>/YYYY-MM-DD.jsonl),
  - decodes APRS packets from their raw bytes with aprslib,
  - stores packets, noise summaries, boots, and errors in SQLite (<data-dir>/hunter.sqlite),
  - prints one line per packet, boot, and error.

Live:    python tools/hunter_log.py --port /dev/cu.usbserial-XXXX --home 59.33,18.06
Replay:  python tools/hunter_log.py --replay logs/2026-11-09.jsonl --data-dir reprocessed
"""

import argparse
import datetime
import json
import math
import pathlib
import sqlite3
import sys
from typing import Optional, Tuple

import aprslib

LORA_APRS_PREFIX = bytes([0x3C, 0xFF, 0x01])
EARTH_RADIUS_KM = 6371.0088

SCHEMA = """
CREATE TABLE IF NOT EXISTS boots (
    id INTEGER PRIMARY KEY,
    host_utc TEXT, fw TEXT, uptime_ms INTEGER, reset_reason INTEGER, radio_status INTEGER,
    freq_hz INTEGER, bw_hz INTEGER, sf INTEGER, cr INTEGER, sync_word INTEGER, preamble INTEGER
);
CREATE TABLE IF NOT EXISTS packets (
    id INTEGER PRIMARY KEY,
    boot_id INTEGER REFERENCES boots(id),
    host_utc TEXT, seq INTEGER, uptime_ms INTEGER, freq_hz INTEGER,
    rssi_dbm REAL, snr_db REAL, freq_err_hz INTEGER, freq_err_ppm REAL,
    crc_ok INTEGER, rx_cr INTEGER, irq INTEGER, modem_stat INTEGER, toa_us INTEGER,
    len INTEGER, raw_hex TEXT,
    src TEXT, dst TEXT, path TEXT, info TEXT, aprs_format TEXT,
    latitude REAL, longitude REAL, altitude_m REAL, speed_kmh REAL, course_deg REAL,
    symbol_table TEXT, symbol TEXT, comment TEXT, weather_json TEXT,
    distance_km REAL, bearing_deg REAL, decode_error TEXT
);
CREATE TABLE IF NOT EXISTS noise (
    id INTEGER PRIMARY KEY,
    boot_id INTEGER REFERENCES boots(id),
    host_utc TEXT, uptime_ms INTEGER, freq_hz INTEGER, samples INTEGER,
    min_dbm REAL, mean_dbm REAL, max_dbm REAL
);
CREATE TABLE IF NOT EXISTS errors (
    id INTEGER PRIMARY KEY,
    boot_id INTEGER REFERENCES boots(id),
    host_utc TEXT, uptime_ms INTEGER, step TEXT, code INTEGER
);
"""


def utc_now() -> str:
    """Current UTC time as ISO 8601 with milliseconds, for example 2026-11-09T12:00:00.000Z."""
    now = datetime.datetime.now(datetime.timezone.utc)
    return now.isoformat(timespec="milliseconds").replace("+00:00", "Z")


def parse_line(line: str) -> Optional[dict]:
    """Return the record on a serial line, or None if the line is not a record.

    The ESP32 prints boot messages that are not JSON; those return None.
    """
    line = line.strip()
    if not line.startswith("{"):
        return None
    try:
        record = json.loads(line)
    except json.JSONDecodeError:
        return None
    if not isinstance(record, dict) or "type" not in record:
        return None
    return record


def decode_aprs(raw_hex: str) -> dict:
    """Decode a LoRa APRS frame from its raw hex.

    Returns the decoded fields, or a dict with only "decode_error" when the
    frame has no LoRa APRS prefix or aprslib cannot parse it.
    """
    data = bytes.fromhex(raw_hex)
    if not data.startswith(LORA_APRS_PREFIX):
        return {"decode_error": "no LoRa APRS prefix"}
    packet = data[len(LORA_APRS_PREFIX):]
    try:
        parsed = aprslib.parse(packet)
    except (aprslib.ParseError, aprslib.UnknownFormat) as error:
        return {"decode_error": "{}: {}".format(type(error).__name__, error)}

    raw = parsed.get("raw", "")
    return {
        "src": parsed.get("from"),
        "dst": parsed.get("to"),
        "path": ",".join(parsed.get("path", [])),
        "info": raw.split(":", 1)[1] if ":" in raw else None,
        "format": parsed.get("format"),
        "latitude": parsed.get("latitude"),
        "longitude": parsed.get("longitude"),
        "altitude_m": parsed.get("altitude"),
        "speed_kmh": parsed.get("speed"),
        "course_deg": parsed.get("course"),
        "symbol_table": parsed.get("symbol_table"),
        "symbol": parsed.get("symbol"),
        "comment": parsed.get("comment"),
        "weather": parsed.get("weather"),
    }


def distance_bearing(lat1: float, lon1: float, lat2: float, lon2: float) -> Tuple[float, float]:
    """Great-circle distance in km and initial bearing in degrees from point 1 to point 2."""
    phi1, phi2 = math.radians(lat1), math.radians(lat2)
    dphi = phi2 - phi1
    dlambda = math.radians(lon2 - lon1)

    a = math.sin(dphi / 2) ** 2 + math.cos(phi1) * math.cos(phi2) * math.sin(dlambda / 2) ** 2
    distance = 2 * EARTH_RADIUS_KM * math.asin(math.sqrt(a))

    y = math.sin(dlambda) * math.cos(phi2)
    x = math.cos(phi1) * math.sin(phi2) - math.sin(phi1) * math.cos(phi2) * math.cos(dlambda)
    bearing = (math.degrees(math.atan2(y, x)) + 360.0) % 360.0
    return distance, bearing


def parse_home(text: str) -> Tuple[float, float]:
    """Parse "lat,lon" in decimal degrees, for example "59.33,18.06".

    Raises argparse.ArgumentTypeError with a readable message, so --home can
    use it as its type and argparse reports bad input as a usage error.
    """
    parts = text.split(",")
    try:
        if len(parts) != 2:
            raise ValueError
        lat, lon = float(parts[0]), float(parts[1])
    except ValueError:
        raise argparse.ArgumentTypeError(
            "expected your position as latitude,longitude in decimal degrees, "
            "for example 59.33,18.06; got '{}'".format(text)) from None
    if not -90.0 <= lat <= 90.0:
        raise argparse.ArgumentTypeError("latitude {} is outside -90 to 90".format(lat))
    if not -180.0 <= lon <= 180.0:
        raise argparse.ArgumentTypeError("longitude {} is outside -180 to 180".format(lon))
    return lat, lon


def open_db(path) -> sqlite3.Connection:
    conn = sqlite3.connect(str(path))
    conn.executescript(SCHEMA)
    return conn


class HunterLog:
    """Archive, decode, and store the records of one serial stream."""

    def __init__(self, conn: sqlite3.Connection, archive_dir: Optional[pathlib.Path] = None,
                 home: Optional[Tuple[float, float]] = None):
        self.conn = conn
        self.archive_dir = archive_dir
        self.home = home
        self.boot_id = None  # id of the last boot record seen; links later records to it

    def handle_line(self, line: str, host_utc: str) -> Optional[str]:
        """Process one serial line. Returns a summary to print, or None.

        A line replayed from an archive keeps the host_utc it was archived with.
        """
        record = parse_line(line)
        if record is None:
            return None
        record_utc = record.pop("host_utc", None) or host_utc
        if self.archive_dir is not None:
            self._archive(record, record_utc)

        kind = record["type"]
        if kind == "rx":
            return self._store_packet(record, record_utc)
        if kind == "noise":
            self._store_noise(record, record_utc)
            return None
        if kind == "boot":
            return self._store_boot(record, record_utc)
        if kind == "error":
            return self._store_error(record, record_utc)
        return None

    def _archive(self, record: dict, host_utc: str) -> None:
        path = self.archive_dir / "{}.jsonl".format(host_utc[:10])
        with open(path, "a", encoding="utf-8") as archive:
            archive.write(json.dumps({"host_utc": host_utc, **record}) + "\n")

    def _store_packet(self, record: dict, host_utc: str) -> str:
        decoded = decode_aprs(record.get("raw_hex", ""))
        freq_hz = record.get("freq_hz")
        freq_err = record.get("freq_err_hz")
        ppm = freq_err / freq_hz * 1e6 if freq_hz and freq_err is not None else None

        distance = bearing = None
        if self.home and decoded.get("latitude") is not None and decoded.get("longitude") is not None:
            distance, bearing = distance_bearing(self.home[0], self.home[1],
                                                 decoded["latitude"], decoded["longitude"])

        weather = decoded.get("weather")
        row = {
            "boot_id": self.boot_id, "host_utc": host_utc,
            "seq": record.get("seq"), "uptime_ms": record.get("uptime_ms"), "freq_hz": freq_hz,
            "rssi_dbm": record.get("rssi_dbm"), "snr_db": record.get("snr_db"),
            "freq_err_hz": freq_err, "freq_err_ppm": ppm,
            "crc_ok": int(bool(record.get("crc_ok"))), "rx_cr": record.get("rx_cr"),
            "irq": record.get("irq"), "modem_stat": record.get("modem_stat"),
            "toa_us": record.get("toa_us"), "len": record.get("len"), "raw_hex": record.get("raw_hex"),
            "src": decoded.get("src"), "dst": decoded.get("dst"), "path": decoded.get("path"),
            "info": decoded.get("info"), "aprs_format": decoded.get("format"),
            "latitude": decoded.get("latitude"), "longitude": decoded.get("longitude"),
            "altitude_m": decoded.get("altitude_m"), "speed_kmh": decoded.get("speed_kmh"),
            "course_deg": decoded.get("course_deg"), "symbol_table": decoded.get("symbol_table"),
            "symbol": decoded.get("symbol"), "comment": decoded.get("comment"),
            "weather_json": json.dumps(weather) if weather else None,
            "distance_km": distance, "bearing_deg": bearing,
            "decode_error": decoded.get("decode_error"),
        }
        self._insert("packets", row)
        return self._packet_summary(row)

    def _store_noise(self, record: dict, host_utc: str) -> None:
        self._insert("noise", {
            "boot_id": self.boot_id, "host_utc": host_utc,
            "uptime_ms": record.get("uptime_ms"), "freq_hz": record.get("freq_hz"),
            "samples": record.get("samples"), "min_dbm": record.get("min_dbm"),
            "mean_dbm": record.get("mean_dbm"), "max_dbm": record.get("max_dbm"),
        })

    def _store_boot(self, record: dict, host_utc: str) -> str:
        radio = record.get("radio", {})
        self.boot_id = self._insert("boots", {
            "host_utc": host_utc, "fw": record.get("fw"), "uptime_ms": record.get("uptime_ms"),
            "reset_reason": record.get("reset_reason"), "radio_status": record.get("radio_status"),
            "freq_hz": radio.get("freq_hz"), "bw_hz": radio.get("bw_hz"), "sf": radio.get("sf"),
            "cr": radio.get("cr"), "sync_word": radio.get("sync_word"), "preamble": radio.get("preamble"),
        })
        return "{} boot: firmware {}, radio status {}, {:.3f} MHz SF{} BW {} kHz CR 4/{}".format(
            host_utc, record.get("fw"), record.get("radio_status"),
            (radio.get("freq_hz") or 0) / 1e6, radio.get("sf"), (radio.get("bw_hz") or 0) // 1000,
            radio.get("cr"))

    def _store_error(self, record: dict, host_utc: str) -> str:
        self._insert("errors", {
            "boot_id": self.boot_id, "host_utc": host_utc, "uptime_ms": record.get("uptime_ms"),
            "step": record.get("where"), "code": record.get("code"),
        })
        return "{} error: {} returned {}".format(host_utc, record.get("where"), record.get("code"))

    def _insert(self, table: str, row: dict) -> int:
        columns = ", ".join(row)
        placeholders = ", ".join("?" for _ in row)
        cursor = self.conn.execute(
            "INSERT INTO {} ({}) VALUES ({})".format(table, columns, placeholders), list(row.values()))
        return cursor.lastrowid

    @staticmethod
    def _packet_summary(row: dict) -> str:
        parts = ["{} #{} {}".format(row["host_utc"], row["seq"], row["src"] or "?")]
        parts.append("{:.1f} dBm, SNR {:.2f} dB, freq err {} Hz".format(
            row["rssi_dbm"], row["snr_db"], row["freq_err_hz"]))
        if row["distance_km"] is not None:
            parts.append("{:.1f} km at {:.0f} deg".format(row["distance_km"], row["bearing_deg"]))
        if not row["crc_ok"]:
            parts.append("CRC FAIL")
        parts.append("[{}]".format(row["aprs_format"] or row["decode_error"]))
        return ", ".join(parts)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--port", help="serial port of the board, for example /dev/cu.usbserial-XXXX")
    source.add_argument("--replay", help="process an archived .jsonl file instead of the serial port")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--data-dir", default="logs", help="where the archive and database go (default: logs)")
    parser.add_argument("--home", type=parse_home,
                        help="your position as latitude,longitude in decimal degrees, for example "
                             "59.33,18.06; adds distance and bearing to stations")
    args = parser.parse_args(argv)

    data_dir = pathlib.Path(args.data_dir)
    data_dir.mkdir(parents=True, exist_ok=True)
    conn = open_db(data_dir / "hunter.sqlite")
    home = args.home

    if args.replay:
        log = HunterLog(conn, archive_dir=None, home=home)
        with open(args.replay, encoding="utf-8") as replay:
            for line in replay:
                summary = log.handle_line(line, utc_now())
                if summary:
                    print(summary)
        conn.commit()
        return 0

    import serial  # only needed for live logging

    log = HunterLog(conn, archive_dir=data_dir, home=home)
    # Opening the port resets the ESP32, so the first record is usually a boot record.
    with serial.Serial(args.port, args.baud, timeout=1) as port:
        print("Listening on {} at {} baud. Press Ctrl+C to stop.".format(args.port, args.baud), flush=True)
        try:
            while True:
                raw = port.readline()
                if not raw:
                    continue
                summary = log.handle_line(raw.decode("utf-8", errors="replace"), utc_now())
                conn.commit()
                if summary:
                    print(summary, flush=True)
        except KeyboardInterrupt:
            pass
    conn.commit()
    return 0


if __name__ == "__main__":
    sys.exit(main())
