# Implementation plan: APRS hunter

A receive-only LoRa APRS receiver on a LilyGO T3 V1.6.1 (433 MHz, SX1278).
The board logs rich metadata for every packet it hears and sends it over USB
serial to a base computer. The base computer archives, decodes, and stores
everything for analysis. Later the board should also work alone, on battery.

## Stage 1: Frame parser and record format
**Goal**: Pure C++ code that splits a LoRa APRS frame into its parts and
formats each event as one JSON line.
**Success Criteria**: Host tests pass with `make test`.
**Tests**:
- A frame with a path splits into source, destination, path, and info.
- A frame without a path has an empty path.
- Info keeps every `:` after the first one.
- Frames without the `3C FF 01` prefix, without `>`, without `:`, or with
  an empty source are rejected with a reason.
- JSON strings escape quotes, backslashes, control bytes, and non-ASCII bytes.
- Packet, noise, boot, and error records format to the expected JSON.
- The received coding rate comes from the modem status register.
- Noise statistics give count, minimum, mean, and maximum.
**Status**: Complete

## Stage 2: Receive-only firmware
**Goal**: An Arduino IDE sketch that listens on 433.775 MHz and prints one
JSON line per packet, a noise summary every 10 seconds, and a boot record.
**Success Criteria**: The sketch builds for the T3 board. On the board it
prints a boot record and noise records.
**Tests**: ESP32 build check. On-board check by the user.
**Status**: Complete

## Stage 3: Base computer logger
**Goal**: A Python script that reads the board over USB serial, adds the
computer's UTC time, archives every line, decodes APRS, and stores the
results in SQLite.
**Success Criteria**: Unit tests pass. A replayed log file fills the database.
**Tests**:
- Non-JSON lines (boot messages from the ESP32) are skipped.
- A packet record is decoded from its raw hex, including position.
- Distance and bearing from a home position are correct.
- Packet, noise, and boot records land in their tables.
**Status**: Complete

## Stage 4: README
**Goal**: Setup, flashing with Arduino IDE, running the logger, and the
record format.
**Status**: Complete

## Verification (2026-11-09)

- Host tests: 32 C++ tests (`make test-firmware`) and 27 Python tests
  (`make test-tools`) pass.
- ESP32 build: the sketch compiles for the T3 board with PlatformIO,
  espressif32 5.2.0 (Arduino core 2.0.5), `-std=gnu++17`, RadioLib 8.0.0.
  23% of flash. Not yet built in Arduino IDE.
- End to end on the computer: records printed by the C++ code, including an
  ESP32 boot line, quotes in a comment, a weather report, and a CRC failure,
  replay through `hunter_log.py` into the expected database rows.
- Not yet verified: running on the board and receiving real packets.
- Stage 5 (display): 82 C++ tests pass, 50 of them new or changed for the
  display logic and records. The sketch builds for the T3 with Adafruit
  SSD1306 2.5.17 and Adafruit GFX 1.13.0, 44% of flash. Not yet verified on
  the board: the drawing, both modes, and both toggles. Known effect:
  opening the serial port within 3 seconds of power-on counts as a double
  reset, because the USB-serial auto-reset and the reset button both pull
  the EN pin.

- Stages 6 and 7 (GPS tagging and web GUI): 123 Python tests pass (27 for
  NMEA and position tracking, 34 for the analysis functions, 21 for the web
  API against a real server, the rest for the logger). The page loaded with
  no script errors in the browser with six hours of generated data (146
  packets, five stations, digipeated and CRC-failed packets, weather, GPS
  tags) and with a copy of the real database (noise only), at desktop and
  phone width. The migration added the new columns to a copy of the real
  database with all rows kept. Not yet verified: a real GPS receiver.

## Stage 5: Wearable display
**Goal**: The OLED shows a short notification for every packet. Between
packets it shows a calm, minimal sonar animation (animated mode) or stays
off (notify-only mode, to save battery). The user can switch modes without a
computer, and the board remembers the mode.
**Success Criteria**: Host tests pass; the sketch builds for the T3 board;
on the board both modes and both toggles work.
**Tests**:
- Screen choice: a notification wins over the splash, the splash wins over
  the idle screen, and notify-only mode turns the screen off when idle.
- Serial commands `display toggle`, `display animated`, and `display notify`
  parse, in any letter case; other text does not.
- A one-shot timer is active only inside its window, and handles the
  32-bit millisecond counter wrapping around.
- The ping ring grows from 0 to its maximum radius once per period.
- A station's sonar dot has a fixed angle for its callsign, and stronger
  signals sit closer to the centre.
- The station table keeps the most recently heard station first, counts
  repeat packets, and drops the oldest station when full.
- Double reset: the first boot is not a double reset; a second boot inside
  the window is; the flag clears after the window and after a detection.
- A packet summary takes the callsign from the frame, or "?" when the frame
  is not valid LoRa APRS.
- The boot record carries the display mode, and a display record reports
  mode changes.
**Status**: In Progress

## Stage 6: Receiver position and GPS time tagging
**Goal**: Every stored packet and noise summary carries the receiver's
position and, when a GPS is attached, a GPS-based UTC time, alongside the
computer's own clock.
**Success Criteria**: Unit tests pass. An existing database gains the new
columns without losing data.
**Tests**:
- NMEA RMC and GGA sentences parse into time, position, altitude,
  satellites, and HDOP; bad checksums and void fixes are rejected.
- The position tracker gives the GPS fix while it is fresh, the fixed home
  position otherwise, and nothing when it has neither.
- GPS time at a packet is the fix time plus the time passed since the fix.
- Packets and noise rows store the receiver position, its source, and the
  GPS time; distance and bearing use the receiver position.
- Opening an old database adds the missing columns.
**Status**: Complete

## Stage 7: Web GUI
**Goal**: A minimal, offline web page that shows the live feed and analyses
everything in the database.
**Success Criteria**: API tests pass; the page loads and draws with the
user's real database and with generated packet data.
**Tests**:
- SX127x IRQ flags and modem status decode into named flags.
- Link margin is SNR minus the demodulator floor for the spreading factor.
- A path with a `*` marks a packet as heard through a digipeater.
- Station statistics: counts, CRC failures, RSSI and SNR ranges, and the
  mean and standard deviation of the frequency error.
- Noise rows are reduced to a set number of points for charts, and
  impossible RSSI readings are flagged.
- The HTTP server answers the API routes with JSON and serves the page.
**Status**: Complete

## Stage 8: Drop torn RSSI reads
**Goal**: Noise windows hold only RSSI readings that were read whole, and
each noise record says how many reads the firmware dropped.
**Success Criteria**: Host and Python tests pass; the firmware builds for
the T3; on the board, with the noise floor near -100 dBm, noise windows show
no -164 dBm minimums and no -37 dBm maximums.
**Tests**:
- A value counts only when two reads in a row agree. A torn first or middle
  read is dropped. Reads that never agree stop at the read limit and skip
  the sample. Every mismatch is counted.
- Noise records carry `read_mismatches` and `skipped_samples`.
- The logger stores both counts, stores NULL for older firmware, and adds
  the columns to an existing database.
- The noise API sums both counts per chart point and per time range.
**Status**: In Progress (code and tests done; needs a run on the board)

## Field observations (2026-11-09, first run on the board)

- 4 boots and 2 double-reset mode switches worked as designed.
- No packets yet in the first 8 minutes.
- Noise mean about -102 dBm with the board next to the computer.
- About once a minute a noise window has a minimum of -164 dBm, which is
  the RSSI register reading 0. That is below thermal noise (about -117 dBm
  at 125 kHz with the SX1278 noise figure), so it is almost certainly a read
  glitch. The window minimum is therefore unreliable; the GUI plots the mean
  and flags these readings. (Superseded 2026-11-13: these are torn reads,
  not a register reading 0. See the next section.)
- Some windows peak at -37 dBm, a very strong signal: likely a nearby
  433 MHz device or local interference. (Wrong, found 2026-11-13: these are
  torn reads too.)
- Windows hold 93-94 samples instead of 100, because screen drawing delays
  the 100 ms sampling.

## Field observations (2026-11-13, new antenna in a window)

- The user moved the board to a window with a better antenna at 07:36 UTC
  (the board restarted). The noise mean rose from about -107 dBm to about
  -101 dBm, so the window gets about 6 dB more noise.
- From then on 20-31% of noise windows had a -164 dBm minimum, against
  under 1% before, and many windows peaked at -37 dBm.
- Both are torn reads of the RSSI register. SPI shifts the byte out top bit
  first. When the register changes during the read, the board gets the top
  bits of one value and the bottom bits of the other. Near -100 dBm the
  register flips between 0x3F (-101 dBm) and 0x40 (-100 dBm), and seven
  bits change at once. The mixes it can produce are 0x00, 0x20, 0x30, 0x38,
  0x3C (-164, -132, -116, -108, -104 dBm) and 0x7F, 0x5F, 0x4F, 0x47, 0x43,
  0x41 (-37, -69, -85, -93, -97, -99 dBm).
- Evidence: in all windows with a mean between -104 and -98 dBm, minimums
  and maximums pile up on exactly those values, and values between them
  almost never appear. -164 dBm and -37 dBm each appear 204 times. Real
  interference would spread over all values.
- The same happens at other bit boundaries with fewer bits changing. On
  2026-11-09 the floor sat near 0x47/0x48 (-93/-92 dBm), and maximums of
  0x4F (-85 dBm) and minimums of 0x40 (-100 dBm) were torn reads too.
- The basement logs had the same effect, in the hours when their floor sat
  near -100 dBm (about 10% of windows on 2026-11-09 13h and 2026-11-10 08h).
- No packet has been received in any log so far.

## Later stages (not planned in detail yet)

- Portable: SD card logging, battery level, lower CPU clock in notify-only
  mode.
- GPS for receiver position and time when away from the computer.
- Analysis: per-station signal history, frequency error per transmitter,
  noise floor over time, coverage map.

## Decisions

- **RadioLib, not arduino-LoRa.** arduino-LoRa drops packets that fail the
  CRC check and hides the radio's status registers. RadioLib still returns
  the bytes of a failed packet and flags the failure, which matters when you
  study weak signals. It also supports the SX1278's FSK and OOK modes, which
  keeps radiosondes and 433.92 MHz sensors open for later. Meshtastic uses
  RadioLib too.
- **Receive only by design.** The sketch never calls a transmit function.
  Sending on 433.775 MHz needs an amateur radio license.
- **Radio settings** come from the defaults of CA2RXU's LoRa_APRS_iGate:
  433.775 MHz, SF12, 125 kHz, coding rate 4/5, CRC on. Sync word `0x12` and
  preamble 8 are the RadioLib defaults that the same firmware relies on.
- **JSON Lines over serial.** One JSON object per line, with a schema
  version field `v`. Any line that is not JSON (ESP32 boot messages) is
  ignored by the logger. The same format can go to an SD card later.
- **Raw bytes are the source of truth.** Every packet record carries the raw
  bytes as hex. The APRS text fields on the board are for display only. The
  logger decodes from the raw hex.
- **Minimal parsing on the board, full decoding on the computer.** The board
  only splits the header, which the OLED will need. The computer decodes
  positions, weather, and Mic-E with `aprslib`.
- **Time.** The board only knows its uptime. The logger adds UTC from the
  computer. Portable use will need GPS for time.
- **Public RadioLib API only.** Raw register access needs RadioLib's
  `RADIOLIB_LOW_LEVEL` switch, which RadioLib warns against. The public API
  gives packet RSSI, SNR, frequency error, IRQ flags, and modem status. Gap:
  the sender's CrcOnPayload flag (register 0x1C) is not logged, so a packet
  sent without CRC would show as `crc_ok`. LoRa APRS senders use CRC.
- **RSSI formula.** RadioLib reports packet RSSI as -164 + PktRssi, plus the
  SNR when the SNR is below 0. The logger can recover the raw register value
  from the logged RSSI and SNR.
- **Noise floor.** The board reads the current channel RSSI every 100 ms and
  reports count, minimum, mean, and maximum every 10 seconds.
- **Torn RSSI reads (2026-11-13).** A noise sample counts only when two
  reads of the RSSI register in a row agree, with at most 4 reads. Each
  noise record counts the mismatches and the samples skipped, so the
  problem stays visible instead of hidden. Rejected alternatives:
  - A wider glitch limit in the analysis. The high-side fakes (-99, -97,
    -93 dBm) look like real readings, so no limit can separate them. And a
    record keeps only the minimum, mean, and maximum, so a bad window cannot
    be repaired on the computer.
  - The median of three reads. It also works, but always costs three reads
    and gives no direct count of torn reads.
  - Matching the tear pattern in old logs. It only works at known bit
    boundaries. Old logs keep the -150 dBm limit, which catches the low side
    only.
- **Minimal sonar for the idle screen.** The user wants to wear the board
  and asked for a minimalistic animation. A first draft had three busy idle
  views (noise graph, radar, station list); it was dropped for one calm
  animation: a ping ring, one dot per station heard in the last 15 minutes,
  and an echo when the ring passes a dot. The moving ring also spreads OLED
  wear, which limits burn-in.
- **Toggles: double reset and a serial command.** The T3 V1.6.1 has only a
  reset button (no user button in LilyGO's pin map). Pressing reset twice
  within 3 seconds switches the mode, which works away from a computer.
  `display toggle` over serial works at the desk. A capacitive touch pin
  was rejected because it needs soldering.
- **Mode and double-reset flag in flash** (ESP32 Preferences). Flash keeps
  them across the reset button and power loss. Two small writes per boot do
  not wear the flash in any meaningful way.
- **Low default brightness and a rotation setting** for wearing: the SSD1306
  contrast is set to 0x40, and `DISPLAY_ROTATION` 2 flips the picture.
- **Adafruit SSD1306 and Adafruit GFX** for drawing: both install from the
  Arduino Library Manager and cover lines, circles, and text.
- **GPS sources: fixed position and NMEA serial.** `--home` gives a fixed
  position. `--gps-port` reads a serial NMEA GPS (for example a USB dongle)
  with pyserial, which the logger already uses. gpsd waits for the choice of
  companion computer. Browser geolocation from the web page was rejected:
  phone browsers only allow it on HTTPS or localhost, and a field server
  serves plain HTTP.
- **Two clocks per record.** `host_utc` is the computer's clock. `gps_utc`
  is the last GPS fix time plus the time passed since that fix, taken from a
  monotonic clock. A portable computer without network time can drift; GPS
  time does not.
- **Web GUI from the Python standard library.** `http.server` plus static
  HTML, CSS, and JavaScript, with charts drawn as SVG. No framework, no CDN,
  no map tiles: it must work offline in the field. A small JSON API reads
  the SQLite database; the page polls it every few seconds. The logger can
  run the server in the same process (`--web`), or `hunter_web.py` runs it
  alone for old data.
- **SQLite in WAL mode** so the web server can read while the logger writes.
- **The station plot is polar**, by bearing and distance from the receiver,
  instead of a map: it needs no tiles and fits the sonar look.
- **Digipeated packets are marked.** Their RSSI and SNR belong to the last
  digipeater, not to the source station, so signal-against-distance analysis
  must not mix them with direct packets.
- **Arduino IDE for building and flashing**, as the user prefers. I check
  builds with PlatformIO from a copy outside the repo; no PlatformIO files
  are part of the repo.
