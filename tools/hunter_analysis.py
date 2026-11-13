"""Analysis helpers for the web GUI. Pure functions over database rows, so
they can be tested without a server."""

import math
import statistics
from typing import Iterable, List, Optional

# SX127x RegIrqFlags (0x12) in LoRa mode, highest bit first.
IRQ_FLAGS = [
    (0x80, "RxTimeout"),
    (0x40, "RxDone"),
    (0x20, "PayloadCrcError"),
    (0x10, "ValidHeader"),
    (0x08, "TxDone"),
    (0x04, "CadDone"),
    (0x02, "FhssChangeChannel"),
    (0x01, "CadDetected"),
]

# SX127x RegModemStat (0x18) bits 4-0. Bits 7-5 hold the received coding rate.
MODEM_STATUS_FLAGS = [
    (0x10, "ModemClear"),
    (0x08, "HeaderInfoValid"),
    (0x04, "RxOngoing"),
    (0x02, "SignalSynchronized"),
    (0x01, "SignalDetected"),
]

# Lowest SNR the SX1276/77/78/79 demodulator can decode, per spreading factor
# (Semtech SX1276/77/78/79 datasheet, table "Range of spreading factors").
DEMODULATOR_FLOOR_DB = {6: -5.0, 7: -7.5, 8: -10.0, 9: -12.5, 10: -15.0, 11: -17.5, 12: -20.0}

# Firmware before 0.3.0 kept torn RSSI reads: the register changed while the
# board read it, so the board got the top bits of one value and the bottom
# bits of the next. Near -100 dBm that gives readings such as -164 dBm (and
# -37 dBm on the high side). Thermal noise in 125 kHz is about -123 dBm, so
# anything below this limit is a torn read. The limit catches only the low
# side; firmware 0.3.0 and later drop torn reads before they count.
RSSI_GLITCH_BELOW_DBM = -150.0


def decode_irq_flags(value: Optional[int]) -> List[str]:
    if value is None:
        return []
    return [name for bit, name in IRQ_FLAGS if value & bit]


def decode_modem_status(value: Optional[int]) -> dict:
    if value is None:
        return {"rx_coding_rate": None, "flags": []}
    field = (value >> 5) & 0x07
    return {
        "rx_coding_rate": "4/{}".format(4 + field) if 1 <= field <= 4 else None,
        "flags": [name for bit, name in MODEM_STATUS_FLAGS if value & bit],
    }


def link_margin_db(snr_db: Optional[float], spreading_factor: Optional[int]) -> Optional[float]:
    """How far the SNR sits above the lowest SNR the radio can decode."""
    if snr_db is None or spreading_factor not in DEMODULATOR_FLOOR_DB:
        return None
    return round(snr_db - DEMODULATOR_FLOOR_DB[spreading_factor], 2)


def heard_via(path: Optional[str]) -> str:
    """The station the receiver actually heard: the last path entry marked
    with '*' (a digipeater that repeated the packet), or "direct"."""
    used = [hop[:-1] for hop in (path or "").split(",") if hop.endswith("*")]
    return used[-1] if used else "direct"


def is_rssi_glitch(rssi_dbm: Optional[float]) -> bool:
    return rssi_dbm is not None and rssi_dbm < RSSI_GLITCH_BELOW_DBM


def _sum_present(rows: List[dict], key: str) -> Optional[int]:
    """Sum of a field over the rows that have it, or None if none have it."""
    values = [r[key] for r in rows if r.get(key) is not None]
    return sum(values) if values else None


def downsample_noise(rows: List[dict], max_points: int) -> List[dict]:
    """Group consecutive noise rows into at most max_points points.

    Each point keeps the first time, the lowest valid minimum, the mean of
    the means, the highest maximum, the total samples, the number of glitch
    minimums left out, and the RSSI reads the firmware dropped (None for
    firmware before 0.3.0).
    """
    if not rows:
        return []
    size = max(1, math.ceil(len(rows) / max_points))
    points = []
    for start in range(0, len(rows), size):
        bucket = rows[start:start + size]
        valid_minimums = [r["min_dbm"] for r in bucket if r["min_dbm"] is not None and not is_rssi_glitch(r["min_dbm"])]
        means = [r["mean_dbm"] for r in bucket if r["mean_dbm"] is not None]
        maximums = [r["max_dbm"] for r in bucket if r["max_dbm"] is not None]
        points.append({
            "host_utc": bucket[0]["host_utc"],
            "samples": sum(r["samples"] or 0 for r in bucket),
            "min_dbm": min(valid_minimums) if valid_minimums else None,
            "mean_dbm": round(statistics.mean(means), 2) if means else None,
            "max_dbm": max(maximums) if maximums else None,
            "glitches": sum(1 for r in bucket if is_rssi_glitch(r["min_dbm"])),
            "read_mismatches": _sum_present(bucket, "read_mismatches"),
            "skipped_samples": _sum_present(bucket, "skipped_samples"),
            "unconfirmed_samples": _sum_present(bucket, "unconfirmed_samples"),
            "rx_lat": bucket[-1].get("rx_lat"),
            "rx_lon": bucket[-1].get("rx_lon"),
        })
    return points


def _mean(values: List[float]) -> Optional[float]:
    return round(statistics.mean(values), 2) if values else None


def station_stats(packets: Iterable[dict]) -> List[dict]:
    """Per-station statistics, most recently heard first.

    Signal figures, the digipeater path, and the frequency error only use
    packets that passed the CRC check, because a damaged packet's header
    may be wrong. The frequency error spread helps tell transmitters apart:
    each crystal has its own steady offset.
    """
    by_station = {}
    for p in packets:
        if p.get("src"):
            by_station.setdefault(p["src"], []).append(p)

    stations = []
    for src, rows in by_station.items():
        good = [r for r in rows if r.get("crc_ok")]
        rssi = [r["rssi_dbm"] for r in good if r.get("rssi_dbm") is not None]
        snr = [r["snr_db"] for r in good if r.get("snr_db") is not None]
        margins = [m for m in (link_margin_db(r.get("snr_db"), r.get("sf")) for r in good) if m is not None]
        ferr = [r["freq_err_hz"] for r in good if r.get("freq_err_hz") is not None]
        ppm = [r["freq_err_hz"] / r["freq_hz"] * 1e6 for r in good
               if r.get("freq_err_hz") is not None and r.get("freq_hz")]
        via = {}
        direct = 0
        for r in good:
            hop = heard_via(r.get("path"))
            if hop == "direct":
                direct += 1
            else:
                via[hop] = via.get(hop, 0) + 1
        positioned = [r for r in good if r.get("latitude") is not None]
        latest = max(good, key=lambda r: r["host_utc"]) if good else None
        latest_position = max(positioned, key=lambda r: r["host_utc"]) if positioned else None
        distances = [r["distance_km"] for r in good if r.get("distance_km") is not None]

        stations.append({
            "src": src,
            "packets": len(rows),
            "crc_fail": len(rows) - len(good),
            "first_utc": min(r["host_utc"] for r in rows),
            "last_utc": latest["host_utc"] if latest else None,
            "rssi_min": min(rssi) if rssi else None,
            "rssi_mean": _mean(rssi),
            "rssi_max": max(rssi) if rssi else None,
            "snr_min": min(snr) if snr else None,
            "snr_mean": _mean(snr),
            "snr_max": max(snr) if snr else None,
            "margin_mean": _mean(margins),
            "ferr_mean": _mean(ferr),
            "ferr_sd": round(statistics.stdev(ferr), 2) if len(ferr) > 1 else None,
            "ppm_mean": _mean(ppm),
            "direct": direct,
            "via": via,
            "latitude": latest_position["latitude"] if latest_position else None,
            "longitude": latest_position["longitude"] if latest_position else None,
            "distance_km": latest_position.get("distance_km") if latest_position else None,
            "bearing_deg": latest_position.get("bearing_deg") if latest_position else None,
            "max_distance_km": max(distances) if distances else None,
            "symbol_table": latest.get("symbol_table") if latest else None,
            "symbol": latest.get("symbol") if latest else None,
            "comment": latest.get("comment") if latest else None,
            "aprs_format": latest.get("aprs_format") if latest else None,
        })
    stations.sort(key=lambda s: s["last_utc"] or "", reverse=True)
    return stations


def packets_per_hour(packets: Iterable[dict]) -> List[dict]:
    hours = {}
    for p in packets:
        hour = p["host_utc"][:13]
        bucket = hours.setdefault(hour, {"hour": hour, "ok": 0, "fail": 0})
        bucket["ok" if p.get("crc_ok") else "fail"] += 1
    return [hours[h] for h in sorted(hours)]


def hexdump(raw_hex: Optional[str]) -> List[str]:
    """Classic hex dump: offset, 16 bytes in hex, and the printable ASCII."""
    data = bytes.fromhex(raw_hex or "")
    lines = []
    for offset in range(0, len(data), 16):
        chunk = data[offset:offset + 16]
        hex_part = " ".join("{:02x}".format(b) for b in chunk)
        text = "".join(chr(b) if 0x20 <= b <= 0x7E else "." for b in chunk)
        lines.append("{:04x}  {}  {}".format(offset, hex_part.ljust(47), text))
    return lines
