"""Serve the APRS hunter web GUI: a static page plus a small JSON API over
the logger's SQLite database. Python standard library only, so it runs
offline on a laptop or a small portable computer.

Alone:        python tools/hunter_web.py --data-dir logs --port 8080
With logger:  python tools/hunter_log.py --port /dev/cu.usbserial-XXXX --web 8080

By default the server only answers on this computer (127.0.0.1). Use
--host 0.0.0.0 to reach it from a phone on the same network; anyone on that
network can then read the data.
"""

import argparse
import datetime
import json
import pathlib
import sqlite3
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import List, Optional
from urllib.parse import parse_qs, urlparse

import hunter_analysis as ha
import hunter_log as hl

WEB_DIR = pathlib.Path(__file__).resolve().parent / "web"
STATIC_FILES = {
    "/": ("index.html", "text/html; charset=utf-8"),
    "/index.html": ("index.html", "text/html; charset=utf-8"),
    "/app.js": ("app.js", "text/javascript; charset=utf-8"),
    "/style.css": ("style.css", "text/css; charset=utf-8"),
}
DEFAULT_SPREADING_FACTOR = 12
MAX_PACKETS = 500
MAX_NOISE_POINTS = 600

PACKET_QUERY = """
SELECT p.*, b.sf AS sf, b.fw AS fw, b.bw_hz AS bw_hz
FROM packets p LEFT JOIN boots b ON b.id = p.boot_id
WHERE p.host_utc >= ?
"""


def connect(db_path) -> sqlite3.Connection:
    conn = sqlite3.connect(str(db_path), timeout=5)
    conn.row_factory = sqlite3.Row
    return conn


def _parse_utc(text: Optional[str]) -> Optional[datetime.datetime]:
    if not text:
        return None
    return datetime.datetime.fromisoformat(text.replace("Z", "+00:00"))


def _seconds_between(later: Optional[str], earlier: Optional[str]) -> Optional[float]:
    a, b = _parse_utc(later), _parse_utc(earlier)
    return round((a - b).total_seconds(), 3) if a and b else None


def enrich_packet(row: sqlite3.Row) -> dict:
    """A packet row plus the values the GUI derives from it."""
    packet = dict(row)
    packet["sf"] = packet.get("sf") or DEFAULT_SPREADING_FACTOR
    packet["heard_via"] = ha.heard_via(packet.get("path"))
    packet["link_margin_db"] = ha.link_margin_db(packet.get("snr_db"), packet["sf"])
    packet["irq_flags"] = ha.decode_irq_flags(packet.get("irq"))
    packet["modem"] = ha.decode_modem_status(packet.get("modem_stat"))
    packet["gps_minus_host_s"] = _seconds_between(packet.get("gps_utc"), packet.get("host_utc"))
    packet["weather"] = json.loads(packet["weather_json"]) if packet.get("weather_json") else None
    return packet


def api_packets(conn: sqlite3.Connection, since: str, limit: int = MAX_PACKETS) -> List[dict]:
    rows = conn.execute(PACKET_QUERY + " ORDER BY p.id DESC LIMIT ?", (since, limit)).fetchall()
    return [enrich_packet(r) for r in rows]


def api_packet(conn: sqlite3.Connection, packet_id: int) -> Optional[dict]:
    row = conn.execute(PACKET_QUERY.replace("WHERE p.host_utc >= ?", "WHERE p.id = ?"), (packet_id,)).fetchone()
    if row is None:
        return None
    packet = enrich_packet(row)
    packet["hexdump"] = ha.hexdump(packet.get("raw_hex"))
    near = conn.execute(
        "SELECT * FROM noise ORDER BY abs(julianday(host_utc) - julianday(?)) LIMIT 1",
        (packet["host_utc"],)).fetchone()
    packet["noise_near"] = dict(near) if near else None
    if near:
        packet["noise_near"]["seconds_from_packet"] = _seconds_between(near["host_utc"], packet["host_utc"])
        packet["noise_near"]["min_is_glitch"] = ha.is_rssi_glitch(near["min_dbm"])
    boot = conn.execute("SELECT * FROM boots WHERE id = ?", (packet.get("boot_id"),)).fetchone()
    packet["boot"] = dict(boot) if boot else None
    return packet


def api_stations(conn: sqlite3.Connection, since: str) -> List[dict]:
    rows = conn.execute(PACKET_QUERY, (since,)).fetchall()
    return ha.station_stats(enrich_packet(r) for r in rows)


def api_noise(conn: sqlite3.Connection, since: str, max_points: int = MAX_NOISE_POINTS) -> List[dict]:
    rows = conn.execute("SELECT * FROM noise WHERE host_utc >= ? ORDER BY id", (since,)).fetchall()
    return ha.downsample_noise([dict(r) for r in rows], max_points)


def api_hourly(conn: sqlite3.Connection, since: str) -> List[dict]:
    rows = conn.execute("SELECT host_utc, crc_ok FROM packets WHERE host_utc >= ?", (since,)).fetchall()
    return ha.packets_per_hour(dict(r) for r in rows)


def api_events(conn: sqlite3.Connection, since: str) -> dict:
    boots = conn.execute("SELECT * FROM boots WHERE host_utc >= ? ORDER BY id DESC LIMIT 50", (since,)).fetchall()
    errors = conn.execute("SELECT * FROM errors WHERE host_utc >= ? ORDER BY id DESC LIMIT 50", (since,)).fetchall()
    return {"boots": [dict(b) for b in boots], "errors": [dict(e) for e in errors]}


def _one(conn: sqlite3.Connection, sql: str, params=()) -> Optional[dict]:
    row = conn.execute(sql, params).fetchone()
    return dict(row) if row else None


def api_summary(conn: sqlite3.Connection, since: str) -> dict:
    counts = _one(conn, """
        SELECT count(*) AS packets, coalesce(sum(crc_ok), 0) AS ok,
               count(DISTINCT CASE WHEN crc_ok = 1 THEN src END) AS stations,
               min(host_utc) AS first_utc, max(host_utc) AS last_utc
        FROM packets WHERE host_utc >= ?""", (since,))
    farthest = _one(conn, """
        SELECT src, distance_km, bearing_deg FROM packets
        WHERE host_utc >= ? AND crc_ok = 1 AND distance_km IS NOT NULL
        ORDER BY distance_km DESC LIMIT 1""", (since,))
    last_packet = _one(conn, """
        SELECT id, host_utc, src, rssi_dbm, snr_db, crc_ok FROM packets
        WHERE host_utc >= ? ORDER BY id DESC LIMIT 1""", (since,))
    last_noise = _one(conn, "SELECT * FROM noise ORDER BY id DESC LIMIT 1")
    if last_noise:
        last_noise["min_is_glitch"] = ha.is_rssi_glitch(last_noise["min_dbm"])
    receiver = _one(conn, """
        SELECT host_utc, gps_utc, rx_lat, rx_lon, rx_alt_m, rx_source, rx_fix_utc, rx_sats, rx_hdop FROM (
            SELECT host_utc, gps_utc, rx_lat, rx_lon, rx_alt_m, rx_source, rx_fix_utc, rx_sats, rx_hdop, id, 1 AS t FROM packets
            UNION ALL
            SELECT host_utc, gps_utc, rx_lat, rx_lon, rx_alt_m, rx_source, rx_fix_utc, rx_sats, rx_hdop, id, 0 AS t FROM noise
        ) ORDER BY host_utc DESC LIMIT 1""")
    if receiver:
        receiver["gps_minus_host_s"] = _seconds_between(receiver["gps_utc"], receiver["host_utc"])
    last_boot = _one(conn, "SELECT * FROM boots ORDER BY id DESC LIMIT 1")
    last_record = _one(conn, """
        SELECT max(t) AS host_utc FROM (
            SELECT max(host_utc) AS t FROM packets UNION ALL SELECT max(host_utc) FROM noise
            UNION ALL SELECT max(host_utc) FROM boots UNION ALL SELECT max(host_utc) FROM errors)""")
    glitches = _one(conn, "SELECT count(*) AS n FROM noise WHERE host_utc >= ? AND min_dbm < ?",
                    (since, ha.RSSI_GLITCH_BELOW_DBM))
    read_check = _one(conn, """
        SELECT sum(read_mismatches) AS mismatches, sum(skipped_samples) AS skipped
        FROM noise WHERE host_utc >= ?""", (since,))
    events = _one(conn, """SELECT (SELECT count(*) FROM boots WHERE host_utc >= ?) AS boots,
                                  (SELECT count(*) FROM errors WHERE host_utc >= ?) AS errors""", (since, since))
    return {
        "since": since,
        "now_utc": hl.utc_now(),
        "packets": counts["packets"],
        "ok": counts["ok"],
        "crc_fail": counts["packets"] - counts["ok"],
        "stations": counts["stations"],
        "first_packet_utc": counts["first_utc"],
        "last_packet": last_packet,
        "farthest": farthest,
        "last_noise": last_noise,
        "noise_glitches": glitches["n"],
        "noise_read_mismatches": read_check["mismatches"],
        "noise_skipped_samples": read_check["skipped"],
        "receiver": receiver,
        "last_boot": last_boot,
        "boots": events["boots"],
        "errors": events["errors"],
        "last_record_utc": last_record["host_utc"] if last_record else None,
    }


def _int_param(query: dict, name: str, default: int, minimum: int, maximum: int) -> int:
    value = int(query.get(name, [default])[0])  # ValueError becomes a 400
    return max(minimum, min(maximum, value))


class HunterRequestHandler(BaseHTTPRequestHandler):
    db_path = None  # set by make_server

    def do_GET(self) -> None:
        url = urlparse(self.path)
        if url.path in STATIC_FILES:
            self._send_static(*STATIC_FILES[url.path])
        elif url.path.startswith("/api/"):
            self._send_api(url.path[len("/api/"):], parse_qs(url.query))
        else:
            self._send_json(404, {"error": "not found"})

    def _send_static(self, name: str, content_type: str) -> None:
        body = (WEB_DIR / name).read_bytes()
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _send_api(self, route: str, query: dict) -> None:
        since = query.get("since", [""])[0]
        try:
            conn = connect(self.db_path)
            try:
                if route == "summary":
                    data = api_summary(conn, since)
                elif route == "packets":
                    data = api_packets(conn, since, _int_param(query, "limit", MAX_PACKETS, 1, 5000))
                elif route == "packet":
                    data = api_packet(conn, _int_param(query, "id", 0, 0, 2**62))
                    if data is None:
                        self._send_json(404, {"error": "no such packet"})
                        return
                elif route == "stations":
                    data = api_stations(conn, since)
                elif route == "noise":
                    data = api_noise(conn, since, _int_param(query, "max_points", MAX_NOISE_POINTS, 10, 5000))
                elif route == "hourly":
                    data = api_hourly(conn, since)
                elif route == "events":
                    data = api_events(conn, since)
                else:
                    self._send_json(404, {"error": "unknown API route"})
                    return
            finally:
                conn.close()
        except ValueError as error:
            self._send_json(400, {"error": "bad parameter: {}".format(error)})
            return
        except sqlite3.Error as error:
            self._send_json(500, {"error": "database: {}".format(error)})
            return
        self._send_json(200, data)

    def _send_json(self, status: int, data) -> None:
        body = json.dumps(data).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, format, *args) -> None:
        pass  # keep the logger's console for packets


def make_server(db_path, host: str = "127.0.0.1", port: int = 8080) -> ThreadingHTTPServer:
    """Create the server. The database must exist; open_db creates it."""
    handler = type("BoundHunterRequestHandler", (HunterRequestHandler,), {"db_path": str(db_path)})
    return ThreadingHTTPServer((host, port), handler)


def start_in_background(server: ThreadingHTTPServer) -> threading.Thread:
    thread = threading.Thread(target=server.serve_forever, name="hunter-web", daemon=True)
    thread.start()
    return thread


def server_url(server: ThreadingHTTPServer) -> str:
    host, port = server.server_address[:2]
    return "http://{}:{}/".format("localhost" if host in ("127.0.0.1", "0.0.0.0") else host, port)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--data-dir", default="logs", help="folder with hunter.sqlite (default: logs)")
    parser.add_argument("--host", default="127.0.0.1", help="0.0.0.0 to allow other devices (default: 127.0.0.1)")
    parser.add_argument("--port", type=int, default=8080)
    args = parser.parse_args(argv)

    db_path = pathlib.Path(args.data_dir) / "hunter.sqlite"
    db_path.parent.mkdir(parents=True, exist_ok=True)
    hl.open_db(db_path).close()  # create it, or add columns an older version lacks
    server = make_server(db_path, args.host, args.port)
    print("APRS hunter GUI on {} (Ctrl+C to stop)".format(server_url(server)), flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
