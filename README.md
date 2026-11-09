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
   {"v":1,"type":"boot","fw":"0.2.0","uptime_ms":15,"reset_reason":1,"radio_status":0,"display":"animated","radio":{"freq_hz":433775000,"bw_hz":125000,"sf":12,"cr":5,"sync_word":18,"preamble":8}}
   {"v":1,"type":"noise","uptime_ms":10015,"freq_hz":433775000,"samples":100,"min_dbm":-121.00,"mean_dbm":-117.40,"max_dbm":-109.00}
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

Then start the logger. `--home` is your position in decimal degrees; it adds
distance and bearing to every station that sends a position.

```bash
.venv/bin/python tools/hunter_log.py --port /dev/cu.usbserial-XXXX --home 59.33,18.06
```

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

-- Noise floor per hour
SELECT substr(host_utc, 1, 13) AS hour, min(min_dbm) AS lowest,
       round(avg(mean_dbm), 1) AS average, max(max_dbm) AS highest
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

### `boot`: once at start-up

Firmware version `fw`, ESP32 `reset_reason`, `radio_status` (RadioLib result
code, 0 is success), the `display` mode (`animated` or `notify`), and the
`radio` settings.

### `display`: a display mode change

The new `mode`, and the `source` that changed it: `serial` or
`double_reset`. The logger keeps these in the archive but not in the
database.

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

This runs the C++ tests on your computer, and the Python tests for the
logger. The C++ tests cover the frame parser, the record format, and the
display logic (screen choice, sonar geometry, station table, double reset).
The drawing code itself needs the board. The first run creates `.venv`.

## Limits

- The board only knows its uptime. Real time comes from the computer.
- The sender's CrcOnPayload flag is not logged, so a packet sent without a
  CRC would show `crc_ok` true. LoRa APRS senders use a CRC.
- One radio listens on one frequency and one set of settings at a time.

`IMPLEMENTATION_PLAN.md` holds the design decisions and the plan for the
next stages: SD card logging and battery level for portable use, GPS, and
deeper analysis.

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
