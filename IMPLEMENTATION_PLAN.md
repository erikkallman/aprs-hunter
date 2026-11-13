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

- Stage 10 (confirm read, 2026-11-13): 140 C++ tests and 144 Python tests
  pass. The sketch builds for the T3 with PlatformIO (espressif32 5.2.0,
  RadioLib 8.0.0), 44% of flash. Not yet built in Arduino IDE and not yet
  run on the board. The length of the 1.5 µs pause is not measured; the
  `t_us` gap before the confirm read in a trace will show it.

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
**Status**: In Progress (read spacing sets the leak rate: 0.3.2 with a
steady 97 µs leaks in 0.8% of windows near -100 dBm, 0.3.1 in 13%, and
the 0.3.3 random pause in 6.4%; 0.3.4 goes back to the 0.3.2 read path
and needs a board run to confirm about 1%. Later on 2026-11-13: the audit
explains these rates, and Stage 10 adds a check that does not depend on
read spacing)

## Stage 9: Trace RSSI reads around torn values
**Goal**: Measure how long a torn value stays in the RSSI register, so the
next fix rests on data instead of a second guess.
**Success Criteria**: Host and Python tests pass; the firmware builds for
the T3; on the board, `rssi_trace` records appear in the daily archive
for samples with a read 6 dB or more from the last noise mean, at most
one per second and 500 per boot.
**Tests**:
- The trace buffer keeps reads in order with their times and ignores
  reads past its capacity.
- A jump is a read at least the given distance above or below the
  reference.
- The gate allows the first trace, refuses traces inside the interval and
  counts them, reports that count with the next trace it allows, and stops
  at the per-boot limit.
- The trace record lists the reads in dBm and their times from the first
  read, and has `value_dbm` null when no two reads agreed.
- The logger archives trace records and stores nothing for them.
**Status**: Complete (493 traces on 2026-11-13; see the observations)

## Stage 10: Confirm far RSSI values with a third read
**Goal**: A torn value cannot pass the read check, whatever the time
between reads is. Each noise record says how many values the confirm read
rejected.
**Success Criteria**: Host and Python tests pass; the firmware builds for
the T3; on the board, with the floor near -100 dBm, no noise window has a
minimum of -164, -132, -116, or -108 dBm or a maximum of -37, -69, or
-85 dBm, and `unconfirmed_samples` is above zero over an hour.
**Tests**:
- A value near the reference, or a reading that is not ok, takes no
  confirm read and comes back as it is.
- A far value takes one confirm read. Within the tolerance it is kept.
  Outside it the reading is not ok, is marked unconfirmed, and counts one
  more mismatch.
- Noise statistics leave out an unconfirmed reading, count it apart from
  skipped samples, and clear the count on reset.
- Noise records carry `unconfirmed_samples`; trace records carry
  `unconfirmed`.
- The logger stores the count, stores NULL for older firmware, and adds
  the column to an existing database. The noise API sums it per chart
  point and per time range.
**Status**: In Progress (firmware 0.3.5 on branch `confirm-far-rssi-reads`;
host tests, Python tests, and the T3 build pass; not yet run on the board)

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

The read check on the board, same window. A window counts as torn when
its minimum is -164, -132, -116, or -108 dBm or its maximum is -37, -69,
-85, or -93 dBm:

| Firmware | Time (UTC) | Windows | Torn windows | Skipped samples |
|---|---|---|---|---|
| 0.2.x | 07:37 to 10:21 | 552 | 78% | none (no check) |
| 0.3.0, 4 reads | 10:21 to 10:29 | 46 | 9% | 15% |
| 0.3.1, 8 reads | from 10:32 | 25 | 8% | 1.3% |

- The check stops about 90% of torn values, not all of them. The ones
  that got through were minimums of -116 dBm (0x30) and -108 dBm (0x38)
  and maximums of -69 dBm (0x5F), all exact tear values.
- An earlier note here said 0.3.0 let no torn values through. That was
  true for its first 9 windows only and is wrong for the whole run.
- To pass the check, two reads in a row must agree on the torn value. A
  tear inside one SPI transfer cannot repeat exactly in the next one. So
  the mixed value must sit in the register itself for at least two reads.
  The SPI explanation above is at best part of the story. How long the
  mixed value stays is not known yet. (Superseded: the 0.3.2 traces below
  show a torn value never lasts past one read.)
- The mean floor barely moved (-101.2 dBm before, -101.3 dBm with the
  check). The torn reads spoiled the minimum and maximum, not the mean.
- The read check made about 94 tries per window, the same as before, so
  it does not slow the loop.
- With 4 reads, there were about 0.9 mismatches per try and 15% of
  samples skipped. That fits a model where each pair of back-to-back
  reads differs with a chance of about 0.5, because the floor sits on the
  -101/-100 dBm step. Most mismatches are normal changes, not tears.
- With that chance, a limit of n reads skips about 0.5^(n-1) of samples:
  14% at 4 reads, 1% at 8. Firmware 0.3.1 uses 8, and the board skipped
  1.3%. Mismatches rose to about 1.1 per try, because samples that 4
  reads would skip now count more mismatches before two reads agree.
- Neither run has a boot record. Opening the port does not reset the
  board, so the logger misses the boot record when it starts after the
  board.

Firmware 0.3.2 traces, 10:47 to 11:11 UTC (493 traces, then the 500 limit;
339 more refused by the one-per-second rule):

- Reads are 97 µs apart (96 to 103). RadioLib's getRSSI is slower than
  the 25 µs assumed earlier. Between reads the register often keeps the
  same value for many reads in a row.
- 903 far values, all exact tear values (-164, -132, -116, -108, -93,
  -85, -69, -37 dBm in about equal numbers). Every one lasted exactly one
  read. The read before and after was back at -101 or -100 dBm.
- So a torn value does not sit in the register. The SPI explanation
  holds, and the note above that said otherwise is wrong.
- About 3% of reads are torn (3.1% in the reads after the stable read,
  which the trigger does not bias).
- Back-to-back torn reads happened 33 times in 13,924 pairs, below the
  52 expected if tears were independent. None of the 33 had the same
  value twice. A torn value passes the check only when two separate tears
  give the same value, which these traces never showed.
- The check returned a far value in 2 traces. Both were a real drop of
  the floor to -107 to -110 dBm at 11:04 that lasted all 28 reads.
- Noise windows from 10:46 on: a torn value in 1 of 222 windows (a -93 dBm
  maximum at 11:16, which can also be real), against 9% on 0.3.0 and 8% on
  0.3.1. Skipped samples 2.1%. The mean floor was -100.1 dBm, against
  -101.3 dBm then.
- Open question: why 0.3.0 and 0.3.1 let more torn values through. The
  floor sat 1 dB lower then, right on the 0x3F/0x40 step, and those runs
  were short (6 torn windows in all). A longer run on 0.3.2 will tell.

Longer run, 10:46 to 13:18 UTC (all firmware counted to the end of its run;
the earlier 0.3.1 count of 2 in 25 covered its first 25 windows only):

| Firmware | Windows | Torn windows | Floor -101 to -100.5 dBm |
|---|---|---|---|
| 0.3.0 | 46 | 4 (8.7%) | 0 of 1 |
| 0.3.1 | 71 | 12 (16.9%) | 5 of 38 |
| 0.3.2 | 907 | 8 (0.9%) | 1 of 178 |

- The floor does not explain the difference. With the floor between -101
  and -100.5 dBm, 0.3.1 leaked in 13% of windows and 0.3.2 in 0.6%.
- 6 of the 8 torn windows on 0.3.2 are a -93 dBm maximum (0x47), which
  may also be real.
- The only change in how 0.3.2 reads the register is that it calls
  micros() and stores each read, a few microseconds per read. The traces
  stopped at 11:11, so printing them is not the cause.
- Hypothesis, not proven: the register updates on a fixed period, and the
  0.3.0/0.3.1 read period locked onto it. Then back-to-back reads tear at
  the same point in the same kind of change and give the same torn value.
  The extra microseconds in 0.3.2 break the lock. The traces cannot show
  this, because they come from 0.3.2 timing.
- Next: firmware 0.3.3 adds a random pause before each read instead of
  testing 0.3.1 again (see the decision on read jitter).

Firmware 0.3.3, 13:32 to 14:36 UTC (384 windows). The pause made leaks
worse, not better:

| Floor (mean dBm) | 0.3.2 torn windows | 0.3.3 torn windows |
|---|---|---|
| -104 to -101.5 | 0 of 39 | 0 of 297 |
| -101.5 to -99.5 | 7 of 865 (0.8%) | 5 of 78 (6.4%) |
| -99.5 to -97 | 1 of 68 | none at this floor |

- Torn values only appear with the floor near the 0x3F/0x40 step, so
  only the middle row compares the two. The gap there has a one-sided
  Fisher p of 0.002. 0.3.3 spent most of its run lower, near -102 dBm,
  which hides the gap in the overall rate (1.3% against 0.8%).
- The traces confirm it. Gaps between reads are now 98 to 204 µs (median
  149) against 96 to 104 µs on 0.3.2. On 0.3.3, 4 of 598 torn values
  repeat on the next read: -37, -37; -116, -116; -85, -85; -37, -37. On
  0.3.2, none of 866 did. (Its one repeat, -108, -108, sat inside a real
  -110 to -108 dBm signal.) 3 of the 4 repeats became the stable value.
- Skipped samples rose from 2.1% to 4.5%, and from 0.8% to 4.7% with the
  floor near -102 dBm. A longer gap gives the register more time to move
  between the two reads, so more pairs disagree.
- What this says about timing: three read spacings give three leak rates
  at the same floor. 0.3.1 (back to back, no micros()) leaked in 13%;
  0.3.2 (a steady 97 µs) in 0.8%; random spacing in 6.4%. So spacing
  matters, as the hypothesis said. But the steady 97 µs spacing is the
  good case, and random spacing averages good and bad spacings. Why 97 µs
  avoids repeats is not known.
- Next: firmware 0.3.4 removes the pause and goes back to the 0.3.2 read
  path. A board run with the floor near -100 dBm should show torn windows
  near 0.8% and skipped samples near 2%.

Audit of the read spacing (2026-11-13, see
`AUDIT_2026-11-13_rssi_read_spacing.md` for the numbers and scripts):

- The RSSI register changes only on a fixed 8.00 µs clock, one sample at
  125 kHz bandwidth. A torn value repeats only when the clock tick lands
  on the same bit of the SPI byte in two reads in a row. That depends on
  the read spacing modulo 8 µs.
- Firmware 0.3.2 reads every 96.87 µs: 12 ticks plus 0.87 µs. Each tear
  moves about 2 bits, so the value differs. This answers "why 97 µs
  avoids repeats". The margin is small: leaks start about 0.4 µs lower.
- The model gives 0% torn windows at 96.87 µs, 6 to 7% for random
  spacing, and 10 to 12% at 96.4 µs. The board gave 0.1%, 5.1%, and 13.4%
  (0.3.1, spacing not measured).
- Correction to the tables above: a -93 dBm maximum is not a torn value
  on its own. On 0.3.2 the window maximums of -96 to -92 dBm form a smooth
  tail of real peaks. Without -93 dBm, 0.3.2 leaked in 1 of 865 windows
  near -100 dBm, not 7. Count torn windows without the -93 dBm maximum
  from here on.
- Firmware 0.3.3 kept running to 14:53 UTC. With the strict rule it
  leaked in 9 of 178 windows near -100 dBm.
- One packet arrived at 14:38:21 UTC (SM4JDW-9, CRC failed, SNR -21 dB).
  It is the first packet in any log.
- Not confirmed: the 0.3.1 spacing, and about 7% of torn pairs that sit
  half a period off the clock.

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
  reads of the RSSI register in a row agree, with at most 8 reads (4 in
  firmware 0.3.0; on the board that skipped 14% of samples, see the
  2026-11-13 observations). Each
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
- **RSSI trace (2026-11-13).** Two reads in a row sometimes agree on a
  torn value, so the SPI explanation was incomplete. Firmware 0.3.2 logs
  the raw reads around each jump before any new fix:
  - The reference is the mean of the last noise window, not the last
    sample. One torn value that got through must not make the next good
    sample look like a jump.
  - A jump is 6 dB or more. In the window runs the floor stayed within
    about 6 dB of the mean, and the torn values that got through (-108,
    -116, -69 dBm) are more than 6 dB from it.
  - After the stable read, the board makes 24 more reads back to back and
    logs every read with its time in microseconds. If a torn value lasts
    through all of them, the next step is reads spaced further apart.
  - At most one trace per second and 500 per boot, so a real signal
    cannot flood the serial line or the archive. Traces refused inside
    the second are counted in the next trace.
  - Rejected: a median of 5 reads now. It fails if a torn value lasts 3
    reads, and nobody knows yet how long one lasts.
- **Read jitter (2026-11-13).** Firmware 0.3.3 waits a random 0 to 100 µs
  before each RSSI read. If the read rhythm can lock onto the register
  updates, a random pause breaks the lock on purpose; 0.3.2 only broke
  it by luck. 100 µs is about one read period, so the pause spans any
  update period shorter than that. It costs at most 0.8 ms per sample.
  - Chosen over running 0.3.1 again for an hour to prove the timing
    cause. The user preferred a fix now. The cost: a low leak rate on
    0.3.3 does not prove the cause, since 0.3.2 is already low.
  - The pause is board timing, so no host test covers it. The board run
    is the test: torn windows should stay near or below 0.9%, with no
    rise in skipped samples.
  - Result: failed on both counts. At the same floor, torn windows rose
    from 0.8% to 6.4% and skipped samples doubled (see the 0.3.3 run in
    the 2026-11-13 observations). The obvious idea, break any lock with
    randomness, was wrong here: the steady spacing of 0.3.2 was the good
    case.
  - Firmware 0.3.4 removes the pause. The read path is the same as 0.3.2:
    one getRSSI, then micros() and a store into the trace buffer. A
    comment on the read function says to keep it so, because a change
    there moves the read spacing and with it the leak rate.
- **Confirm read for far values (2026-11-13).** Firmware 0.3.5 checks an
  agreed value that lies 6 dB or more from the last noise mean with one
  more read, made after a 1.5 µs pause. The value counts only when that
  read is within 2 dB of it.
  - Why: the audit shows that a torn value repeats when the read spacing
    is close to a multiple of the register's 8 µs clock. The 0.3.4 fix
    (keep the read path frozen at 96.87 µs) works by a margin of under a
    microsecond that the compiler sets. The pause moves the clock tick
    about 3 bits along the SPI byte, so a third identical tear cannot
    happen at any spacing.
  - 1.5 µs keeps the third read away from the clock (0 and 8 µs) and from
    the half-period minority (4 µs), both at the present spacing and at a
    locked one: the tick moves 1.5 to 2.4 µs.
  - The pause counts CPU cycles. delayMicroseconds() is only good to about
    a microsecond, which is the whole margin.
  - Only far values take the third read, so normal samples cost nothing
    more. 6 dB is the distance that already starts a trace.
  - The confirm read needs to be within 2 dB, not equal. Two clean reads
    in a row agree only 40 to 47% of the time, so an exact match would
    drop about half of all real far signals. Torn values lie 6 dB or more
    from the floor and 8 dB or more from each other, so 2 dB cannot let
    one through.
  - A rejected value counts as `unconfirmed_samples`, apart from
    `skipped_samples`, so the board run can show the check at work.
  - Known limits: a real spike shorter than three reads (about 0.3 ms) is
    dropped. Torn values within 6 dB of the floor (-104, -102, -99,
    -97 dBm) still pass; they barely move the minimum and maximum.
  - A trace now makes 23 extra reads, not 24, so the stable read, the
    confirm read, and the extra reads fit the 32-read buffer.
  - Rejected: tuning the spacing to 98 or 102 µs. It works in the model
    but still depends on the build. Rejected: a faster SPI clock. It only
    makes tears rarer and is not tested on this board.
  - Rejected for now: a mean read gap in each noise record. With the
    confirm read the spacing no longer decides the result, and the traces
    already carry read times.
  - Not done: a board run at 96.0 µs on purpose, which would prove the
    model (it predicts torn windows near 40% without the confirm read).
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
