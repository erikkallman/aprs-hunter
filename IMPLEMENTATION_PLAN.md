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

## Later stages (not planned in detail yet)

- Portable: OLED status screen, SD card logging, battery level.
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
- **Arduino IDE for building and flashing**, as the user prefers. I check
  builds with PlatformIO from a copy outside the repo; no PlatformIO files
  are part of the repo.
