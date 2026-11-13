# APRS hunter

A receive-only LoRa APRS logger for the LilyGO T3 V1.6.1 (433 MHz version,
SX1278 radio).

The board listens on 433.775 MHz and prints one JSON line for every packet it
hears. Each line holds the signal metadata (RSSI, SNR, frequency error, coding
rate, CRC result, time on air) and the raw bytes. A script on your computer
reads these lines over USB, stores them, decodes the APRS content, and puts
everything in a SQLite database for analysis.

The board is currently set up to never transmit. Sending on 433.775 MHz needs an amateur radio
license in Sweden. Receiving does not. Be mindful of how you use the radio spectrum.

The code is provided as is, under the MIT license. You are responsible for
using it and your radio hardware lawfully. See
[License and responsible use](#license-and-responsible-use).

## What you need

- A LilyGO T3 V1.6.1, 433 MHz version (SX1278), with a 433 MHz antenna.
  Attach the antenna before you power the board.
- Arduino IDE with the ESP32 board package ("esp32 by Espressif Systems").
- These libraries, from the Arduino Library Manager:
  - "RadioLib" by Jan Gromes, version 8.0 or newer.
  - "Adafruit SSD1306" by Adafruit. When the Library Manager offers to
    install its dependencies (Adafruit GFX Library, Adafruit BusIO), accept.
- Python 3.9 or newer on the computer, for the logger.

## Flash the board

1. Open `aprs_hunter/aprs_hunter.ino` in Arduino IDE.
2. Pick *Tools > Board > esp32 > TTGO LoRa32-OLED*, then
   *Tools > Revision > TTGO LoRa32 V2.1 (1.6.1)*.
3. Set *Tools > Upload Speed* to 115200. Boards with a WCH CH9102 USB chip
   fail to upload at 921600 on macOS.
4. Pick the port, for example `/dev/cu.usbserial-…`, and click Upload.
5. Open the serial monitor at 115200 baud. You should see a boot line, then a
   noise line every 10 seconds:

   ```
   {"v":1,"type":"boot","fw":"0.3.5","uptime_ms":15,"reset_reason":1,"radio_status":0,"display":"animated","radio":{"freq_hz":433775000,"bw_hz":125000,"sf":12,"cr":5,"sync_word":18,"preamble":8}}
   {"v":1,"type":"noise","uptime_ms":10015,"freq_hz":433775000,"samples":94,"read_mismatches":0,"skipped_samples":0,"unconfirmed_samples":0,"min_dbm":-109.00,"mean_dbm":-106.50,"max_dbm":-104.00}
   ```

   `radio_status` 0 means the radio started. Any other number is a RadioLib
   error code.

The board LED flashes on every received packet.

## The display

The OLED is built to be worn: it shows little, moves slowly, and stays dim.

**On every packet**, in both modes, it shows a notification for 4 seconds: a
short burst of rings, then the callsign in large letters, the RSSI and SNR,
and the packet number. A packet that failed the CRC check shows `CRC FAIL`,
and a frame that is not LoRa APRS shows `?` as the callsign.

**Between packets**, it depends on the mode:

- **Animated** (default): a minimal sonar. A ring grows out from the centre
  every 2.6 seconds. Every station heard in the last 15 minutes is a dot. The
  dot's angle comes from the callsign, so a station keeps its place. Stronger
  signals sit closer to the centre. A dot is bigger for the first minute after
  a packet, and it lights up when the ring passes over it. The packet count
  sits in the bottom-right corner.
- **Notify-only**: the screen is off, to save battery. It only wakes for
  notifications.

At every start, the screen shows the current mode for 2.5 seconds.

### Switching the mode

- **Press reset twice within 3 seconds.** The board has no other button. The
  board restarts on each press, and the second start switches the mode and
  shows it on the screen.
- **Or send a command over serial**, for example in the Arduino serial
  monitor: `display toggle`, `display animated`, or `display notify`. The
  board answers with a `display` record.

The board saves the mode in flash, so it survives power-off.

Opening the serial port also restarts the board. If you open it within 3
seconds of powering the board on, the board counts that as a double reset and
switches the mode. Wait a few seconds after power-on, or switch back.

### Settings

At the top of `aprs_hunter/aprs_hunter.ino`:

- `DISPLAY_CONTRAST`: brightness from 0 to 255. The default 0x40 is dim; a
  lower value also uses less power.
- `DISPLAY_ROTATION`: set it to 2 to turn the picture upside down, for
  example when you wear the board with the USB port up.
- `NOTIFICATION_MS`: how long a notification stays on screen.

## Run the logger

Close the Arduino serial monitor first: only one program can use the port.

Set up the Python environment once:

```bash
make venv
```

Then start the logger. Replace `59.33,18.06` with your own position in
decimal degrees, or leave `--home` out. `--web 8080` also serves the web GUI
at http://localhost:8080/.

```bash
.venv/bin/python tools/hunter_log.py --port /dev/cu.usbserial-XXXX --home 59.33,18.06 --web 8080
```

All options:

| Option | What it does |
|---|---|
| `--port` | The board's serial port. |
| `--baud` | Board baud rate (default 115200). |
| `--data-dir` | Where the archive and database go (default `logs`). |
| `--home LAT,LON` | Fixed receiver position, used when no GPS fix is available. |
| `--gps-port` | Serial port of an NMEA GPS, for example a USB GPS dongle. |
| `--gps-baud` | GPS baud rate (default 9600). |
| `--web PORT` | Also serve the web GUI on this port. |
| `--web-host` | `0.0.0.0` lets other devices, such as your phone, open the GUI. Anyone on that network can then read the data. Default `127.0.0.1`: this computer only. |
| `--replay FILE` | Process an archive file instead of the serial port. |

Opening the port resets the board, so the first line is usually a boot line.
The logger prints one line per packet:

```
2026-11-09T12:16:21.212Z #1 N0CALL-7, -118.8 dBm, SNR -12.50 dB, freq err 2100 Hz, 37.3 km at 6 deg, [uncompressed]
```

It writes two things to `logs/`:

- `YYYY-MM-DD.jsonl`: every record from the board, with the computer's UTC
  time added as `host_utc`. This archive is the complete raw record.
- `hunter.sqlite`: the decoded database.

To rebuild a database from an archive, for example after a logger change:

```bash
.venv/bin/python tools/hunter_log.py --replay logs/2026-11-09.jsonl --data-dir reprocessed --home 59.33,18.06
```

### Receiver position and time

Each packet and noise row records where the receiver was and what time it
was, from two clocks:

- `host_utc`: the computer's clock.
- `gps_utc`: GPS time. It is the time of the last GPS fix plus the time passed
  since that fix. A portable computer without internet can drift; GPS time
  does not. It stays valid for an hour after the GPS loses its fix, for
  example indoors.

The receiver position comes from a GPS fix younger than 10 seconds. Without
one, it comes from `--home`. The source is stored in `rx_source` (`gps` or
`fixed`), together with `rx_lat`, `rx_lon`, `rx_alt_m`, `rx_fix_utc`,
`rx_sats`, and `rx_hdop`. Distance and bearing to each station are measured
from the receiver position.

For a GPS, use any NMEA receiver that shows up as a serial port, for example
a u-blox USB dongle, and pass its port with `--gps-port`. The logger reads the
RMC and GGA sentences. The archive keeps the position and GPS time of every
record, so `--replay` reproduces them.

The phone's own GPS cannot be used through the web page: phone browsers only
give a web page your location over HTTPS, and the logger serves plain HTTP.

## Web GUI

Open http://localhost:8080/ after you start the logger with `--web 8080`. To
look at old data without the board, run the GUI on its own:

```bash
.venv/bin/python tools/hunter_web.py --data-dir logs --port 8080
```

The page works offline: it uses no internet and no outside libraries. It
updates every 5 seconds. The buttons at the top pick the time range.

- **Tiles**: packet and station counts, CRC failure rate, farthest station,
  last packet, the latest noise window and the RSSI reads the firmware
  dropped, the receiver position and its
  source, GPS time minus computer time, and the board's firmware and radio
  settings. The dot next to the title is green while records arrive.
- **Noise floor and packets**: the noise mean as a line, the minimum to
  maximum as a band, every packet as a dot at its RSSI (hollow for a CRC
  failure), and ticks for torn -164 dBm readings in logs from firmware
  before 0.3.0, which are left out of the band.
- **Stations by bearing and distance**: a compass plot around the receiver.
  Hollow dots were only heard via a digipeater.
- **Signal against distance**: RSSI against the distance to each station.
  Hollow dots came via a digipeater, so their RSSI belongs to the
  digipeater, not the station.
- **SNR**: a histogram with the lowest SNR the radio can decode at the
  current spreading factor (-20 dB at SF12).
- **Packets per hour**, with CRC failures in red.
- **Frequency error per station**: each transmitter's crystal has its own
  offset, so a steady value tells transmitters apart.
- **Stations table**, sortable: counts, last heard, RSSI and SNR ranges,
  link margin, frequency error with spread, distance, bearing, and how the
  station was heard (direct or via which digipeater).
- **Packets table**: click a row for every detail of that packet: signal,
  link margin, decoded IRQ flags and modem status, the nearest noise window,
  the decoded APRS fields and weather, the receiver position, both clocks,
  the board uptime and firmware, and a hex dump of the raw bytes.
- **Boots and errors**.

## Analyse the data

The database has four tables: `packets`, `noise`, `boots`, and `errors`.
Every row has `host_utc`, and rows that come after a boot record link to it
through `boot_id`. Some starting points, using the `sqlite3` tool that comes
with macOS:

```sql
-- Stations heard, with packet count, best signal, and distance
SELECT src, count(*) AS packets, max(rssi_dbm) AS best_rssi, max(snr_db) AS best_snr,
       round(max(distance_km), 1) AS max_km
FROM packets WHERE crc_ok = 1 GROUP BY src ORDER BY packets DESC;

-- Frequency error per station. Each transmitter's crystal has its own
-- offset, so a steady value helps tell transmitters apart.
SELECT src, count(*) AS packets, round(avg(freq_err_hz)) AS mean_err_hz,
       round(avg(freq_err_ppm), 2) AS mean_ppm
FROM packets WHERE crc_ok = 1 GROUP BY src;

-- Noise floor per hour, with the RSSI reads the firmware dropped. Minimums
-- below -150 dBm are torn reads from firmware before 0.3.0.
SELECT substr(host_utc, 1, 13) AS hour,
       min(CASE WHEN min_dbm >= -150 THEN min_dbm END) AS lowest,
       round(avg(mean_dbm), 1) AS average, max(max_dbm) AS highest,
       sum(read_mismatches) AS reads_dropped
FROM noise GROUP BY hour;

-- Good packets and CRC failures per day
SELECT substr(host_utc, 1, 10) AS day, sum(crc_ok) AS good, sum(1 - crc_ok) AS crc_fail
FROM packets GROUP BY day;
```

Run one with `sqlite3 -header -column logs/hunter.sqlite "<query>"`.

## Record format

The board prints one JSON object per line. `v` is the format version and
`type` says what the line is. Lines that are not JSON, such as ESP32 boot
messages, are ignored by the logger.

### `rx`: a received packet

| Field | Meaning |
|---|---|
| `seq` | Packet number since boot, starting at 1 |
| `uptime_ms` | Board uptime when the radio signalled the packet |
| `freq_hz` | Receive frequency |
| `rssi_dbm` | Packet signal strength. RadioLib computes -164 + PktRssi, plus the SNR when the SNR is below 0 |
| `snr_db` | Packet signal-to-noise ratio. LoRa decodes down to about -20 dB at SF12 |
| `freq_err_hz` | Offset between the sender's frequency and ours |
| `crc_ok` | `false` if the packet failed the CRC check. The bytes are still logged |
| `rx_cr` | Coding rate from the received header: 5 means 4/5. `null` if unknown |
| `irq` | SX1278 IRQ flags register, read before it is cleared |
| `modem_stat` | SX1278 modem status register |
| `toa_us` | Time on air for this packet length at the configured settings |
| `len` | Packet length in bytes |
| `raw_hex` | The received bytes, as hex. The logger decodes from this field |
| `aprs` | Header fields split on the board: `src`, `dst`, `path`, `info`. For display only; non-ASCII bytes appear as `\u00XX` |
| `aprs_error` | Present instead of `aprs` when the frame is not valid LoRa APRS |

### `noise`: a noise floor summary, every 10 seconds

`samples` readings of the channel RSSI, taken every 100 ms, with `min_dbm`,
`mean_dbm`, and `max_dbm`. Packets received during the window raise
`max_dbm`.

The radio changes its RSSI register all the time, so a read can catch it
halfway through a change (a torn read, see Limits). For each sample the board
reads the register until two reads in a row agree, at most 8 times (4 in
firmware 0.3.0). Firmware 0.3.3 waited a random 0 to 100 µs before each
read; that let more torn values through, and 0.3.4 removed it.

Two reads can still agree on a torn value (see Limits). So from firmware
0.3.5 on, a value 6 dB or more from the mean of the last noise window
needs a confirm read: the board pauses 1.5 µs, reads once more, and keeps
the value only if that read is within 2 dB of it. A real signal lasts and
passes. A torn value does not.

These fields count what the check found:

| Field | From | Meaning |
|---|---|---|
| `read_mismatches` | 0.3.0 | How often a read disagreed with the read before it. Each mismatch drops one read |
| `skipped_samples` | 0.3.0 | Samples left out because no two reads in a row agreed |
| `unconfirmed_samples` | 0.3.5 | Samples left out because two reads agreed on a far value and the confirm read did not |

`samples`, `skipped_samples`, and `unconfirmed_samples` add up to the
number of tries in the window. The logger stores NULL in a column for
records from firmware that does not send it.

Each unconfirmed sample is most likely a torn value that the older
firmware would have counted. With the floor near -100 dBm, expect a few
per hour. A real spike shorter than about 0.3 ms also ends up here.

Most mismatches are not torn reads. When the noise floor sits on the step
between two register values, the value often changes between two reads.
With the floor near -100 dBm, about half of all back-to-back reads differ,
so expect about one mismatch per sample. A high `read_mismatches` count is
normal. With 8 reads, expect about one skipped sample per window. Many more
means the value changes faster than the check can follow.

### `boot`: once at start-up

Firmware version `fw`, ESP32 `reset_reason`, `radio_status` (RadioLib result
code, 0 is success), the `display` mode (`animated` or `notify`), and the
`radio` settings.

### `display`: a display mode change

The new `mode`, and the `source` that changed it: `serial` or
`double_reset`. The logger keeps these in the archive but not in the
database.

### `rssi_trace`: the raw reads around a jump

From firmware 0.3.2 on. These records exist to find out how long a torn
value stays in the RSSI register (see Limits). When a read of a noise
sample lands 6 dB or more from the mean of the last noise window, the
board makes 23 more reads (24 before firmware 0.3.5) and prints every
read of that sample:

```
{"v":1,"type":"rssi_trace","uptime_ms":61200,"ref_dbm":-101.32,"ok":true,"value_dbm":-69.00,"unconfirmed":false,"stable_reads":3,"missed":0,"reads_dbm":[-101,-69,-69,-69,-100,...],"t_us":[0,26,51,77,102,...]}
```

| Field | Meaning |
|---|---|
| `ref_dbm` | The mean of the last noise window, which the reads were compared with |
| `ok`, `value_dbm` | What the read check returned. `value_dbm` is null when no two reads agreed, or when the confirm read rejected the value |
| `unconfirmed` | From firmware 0.3.5. True when two reads agreed on a far value and the confirm read rejected it |
| `stable_reads` | How many of the reads the read check made, with the confirm read if there was one. The rest came after it |
| `missed` | Traces left out since the last one, because they came within a second of it |
| `reads_dbm` | Every read, in order |
| `t_us` | When each read finished, in microseconds after the first one |

The board prints at most one trace per second and 500 per boot. To turn
traces off, set `RSSI_TRACE_LIMIT` to 0 in the sketch. The logger keeps
these records in the archive but not in the database.

### `error`

`where` names the step that failed, and `code` is its result code. For radio
steps the code is a RadioLib result code. `display.begin` means the screen
did not answer. `serial: unknown command` means the board did not understand
a serial command.

## Radio settings

The sketch uses the European LoRa APRS settings: 433.775 MHz, SF12, 125 kHz,
coding rate 4/5, sync word `0x12`, preamble 8, CRC on. They match the
defaults of CA2RXU's LoRa_APRS_iGate firmware. To listen elsewhere, change the
`APRS_*` defines at the top of `aprs_hunter/aprs_hunter.ino`.

## Tests

```bash
make test
```

This runs the C++ tests on your computer, and the Python tests. The C++ tests
cover the frame parser, the record format, the RSSI read check and trace,
and the display logic (screen choice, sonar geometry, station table,
double reset). The drawing code
itself needs the board. The Python tests cover the logger, NMEA parsing and
position tracking, the analysis functions, and the web server's API with a
real server on a free port. The first run creates `.venv`.

## Limits

- The board only knows its uptime. Real time comes from the computer or a
  GPS on the computer.
- The GPS support is tested with recorded NMEA sentences, not yet with a GPS
  receiver.
- Firmware before 0.3.0 logged torn RSSI reads. The radio changes its RSSI
  register while the board reads it, so the board gets the top bits of one
  value and the bottom bits of the next. This is worst near -100 dBm, where
  the register flips from 0x3F to 0x40 and seven bits change at once: a
  -101 dBm reading comes out as -164, -132, or -116 dBm, or as -37, -69, or
  -85 dBm. Firmware 0.3.0 drops most of these reads. On the board, the
  share of noise windows with a torn minimum or maximum fell from 78% to
  about 8%, and in firmware 0.3.2 to about 0.1%. The ones left
  pass because two reads in a row return the same torn value. Traces
  (`rssi_trace`) show that a torn value lasts one read, so this needs two
  separate tears that give the same value. The register changes on a fixed
  8 µs clock, so that happens when the time between two reads is close to
  a multiple of 8 µs. Firmware 0.3.2 reads every 96.87 µs, which is clear
  of 96.0 µs by under a microsecond. Firmware 0.3.5 adds a confirm read so
  the result no longer depends on that spacing (see
  `AUDIT_2026-11-13_rssi_read_spacing.md`). It is not yet proven on the
  board. In older logs the GUI flags minimums below -150 dBm and
  leaves them out of the noise band, but the fake high readings stay in
  the maximum.
- The sender's CrcOnPayload flag is not logged, so a packet sent without a
  CRC would show `crc_ok` true. LoRa APRS senders use a CRC.
- One radio listens on one frequency and one set of settings at a time.

`IMPLEMENTATION_PLAN.md` holds the design decisions and the plan for the
next stages, such as SD card logging and battery level for portable use.

## License and responsible use

This project is released under the MIT license. See `LICENSE`.

The software is provided "as is", without warranty of any kind. The authors
are not liable for any claim, damage, or other liability that comes from its
use.

You are responsible for using this software and your radio hardware correctly
and in line with the law where you use it. That covers:

- the frequencies you listen on and transmit on,
- transmit power and duty cycle limits,
- any license that transmitting needs,
- any rules on recording or sharing what you receive.

The firmware in this repository only receives. If you change it to transmit,
check the rules of your national radio authority first. In Sweden that is
Post- och telestyrelsen (PTS).

`tests/doctest.h` is the doctest testing framework by Viktor Kirilov,
included under its own MIT license. See the notice at the top of that file.
