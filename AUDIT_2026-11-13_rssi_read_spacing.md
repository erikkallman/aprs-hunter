# Audit: why the 97 µs read spacing works

This is an additional, independent audit of the torn RSSI read work in
`IMPLEMENTATION_PLAN.md` (Stage 8, Stage 9, and the 2026-11-13 field
observations). It does not replace the plan. It checks the plan's claims
against the raw log and adds one finding the plan does not have.

| | |
|---|---|
| Audit time | 2026-11-13T15:07Z |
| Repo commit | `849b612caed9f2afc3941d5f5a05a69ad9a86c07` (`main`, "Remove the random pause before each RSSI read", firmware 0.3.4 in source) |
| Working tree | clean at audit time |
| Data | `logs/2026-11-13.jsonl`, 5835 lines, last record 2026-11-13T14:53:23Z, SHA-256 starts `97f6661d90a12769`. The log is not in git. |
| Scope | Analysis only. The audit changed no code and no plan text. |
| Board runs used | 0.3.1 (commit `769a808`), 0.3.2 (`9a0e78f`), 0.3.3 (`1bf9661`). No 0.3.4 data existed at audit time. |

## Summary

1. The RSSI register changes only on a fixed clock of 8.00 µs. That is one
   sample period at 125 kHz bandwidth.
2. A read tears when a clock tick lands inside the SPI data byte (8 bits of
   0.5 µs at 2 MHz).
3. A torn value passes the read check only when the next read tears in the
   same bit slot. That depends on the read spacing modulo 8 µs, not on the
   spacing itself.
4. Firmware 0.3.2 reads every 96.87 µs. That is 12 ticks plus 0.87 µs, so
   each tear moves about 2 bits and the torn value never repeats.
5. 97 µs is not a good setting in itself. It sits 0.87 µs clear of a bad
   spacing at 96.0 µs. Leaks start about 0.4 µs below the present spacing.
6. The plan's 0.8% leak rate for 0.3.2 is too high. Most of those windows
   are real signal peaks at -93 dBm. The real rate is about 0.1%.

## Plan claims and the audit verdict

| Claim in the plan | Verdict |
|---|---|
| Torn values are SPI tears, top bits old and bottom bits new | Holds. Tear timing fits the bit position to within 1 µs. |
| A torn value lasts one read | Holds. |
| "Read spacing sets the leak rate" | Holds, but the cause is the spacing modulo 8 µs. |
| "The register updates on a fixed period and the 0.3.1 read period locked onto it" (hypothesis) | Supported. The period is 8.00 µs. The 0.3.1 spacing itself is not measured. |
| "0.3.1 read back to back", as the difference from 0.3.2 | Misleading. 0.3.2 also reads back to back. The difference is a fraction of a microsecond per read. |
| "Why 97 µs avoids repeats is not known" | Now explained, see finding 3. |
| "Random spacing averages good and bad spacings" | Holds. About 1 gap in 16 puts the tick in the same bit slot. |
| 0.3.2 leaks in 0.8% of windows near -100 dBm | Too high. 6 of its 8 torn windows are real -93 dBm peaks. |
| A longer gap makes more read pairs disagree | Holds. Agreement falls from 47% to 40%. |

## Findings

### 1. The register updates every 8.00 µs

A torn value tells where in the data byte the register changed. Call `k`
the number of bits that came from the old value:

| k | Low side (0x3F to 0x40) | High side (0x40 to 0x3F) |
|---|---|---|
| 2 | 0x00, -164 dBm | 0x7F, -37 dBm |
| 3 | 0x20, -132 dBm | 0x5F, -69 dBm |
| 4 | 0x30, -116 dBm | 0x4F, -85 dBm |
| 5 | 0x38, -108 dBm | 0x47, -93 dBm |

For two torn reads in one trace, the time between the two register changes
is `D = (t_j - t_i) + 0.5 * (k_j - k_i)` µs. If the register changes on a
clock of period T, every D is a multiple of T.

On 0.3.3 the read gaps are random (98 to 204 µs), so D takes many values.
That makes a period search possible:

- 240 torn pairs. The Rayleigh power peaks at T = 8.000 µs with power 142
  (mean resultant 0.77). The next unrelated peak has power 10.
- 214 of 240 pairs have D within 1 µs of a multiple of 8.
- Power at T = 16 µs is 1.2, so the register can change on every 8 µs tick,
  not every second one.
- Control: 1434 pairs of clean reads give power 0.3 at T = 8.

On 0.3.2, 453 of 532 torn pairs have D within 1 µs of a multiple of 8.

### 2. The 0.3.2 read spacing is 96.87 µs

The logged gaps on 0.3.2 are whole microseconds: 9292 at 97, 1379 at 96,
and 2361 from 98 to 104. The 98 to 104 gaps come about once a millisecond
and fit a timer interrupt of 4 to 5 µs. Among the 96 and 97 gaps, 13% are
96, so the true spacing is 96.87 µs with little jitter.

96.87 = 12 x 8 + 0.87. Each read starts 0.87 µs later against the register
clock. A tick that lands on bit k in one read lands about 1.7 bits earlier
in the next read.

The traces show this directly. Of 30 back-to-back torn pairs with different
values on 0.3.2, 21 moved 2 bits earlier, 8 moved 1 bit earlier, and 1
moved 3 bits earlier. None stayed in place or moved later.

### 3. Leak rate against read spacing

A torn value repeats only when the tick lands in the same bit slot twice.
For a steady spacing P:

- P within about 0.5 µs of a multiple of 8 µs (96.0, 104.0): the tear
  repeats often. This is the worst case.
- P anywhere else: the tear cannot repeat on the main clock.
- Random spacing: about 1 gap in 16 repeats.

A simulation of this model (appendix) against the board, for windows with
the floor between -101.5 and -99.5 dBm:

| Read spacing | Model, torn windows | Board, torn windows |
|---|---|---|
| Steady 96.87 µs (0.3.2) | 0% | 0.1% (1 of 865) |
| Random 98 to 198 µs (0.3.3) | 6 to 7% | 5.1% (9 of 178) |
| Steady 96.40 µs | 10 to 12% | 13.4% (9 of 67) on 0.3.1, spacing not measured |
| Steady 96.20 µs | 29% | not run |
| Steady 96.00 µs | 42% | not run |
| Steady 104.0 µs | 44% | not run |
| Steady 98.0 or 100.0 µs | 0% | not run |

The board column uses the strict rule from finding 4. The 0.3.3 run
continued to 14:53 UTC, so it has more windows than the plan's count of 78.

The model has one fitted number: the chance that the register crosses the
0x3F/0x40 step on a tick, 12.4%. It comes from the 3.1% of reads that tear
on 0.3.2. The model column is a random simulation of 400,000 samples per
row. Two runs differ by about one percentage point, hence the ranges.

Three of the four real repeats on 0.3.3 had gaps of 152, 176 and 169 µs,
all within 1 µs of a multiple of 8. The fourth had 181 µs and does not fit.

### 4. The -93 dBm maximums on 0.3.2 are real signals

The plan counts a window as torn when its maximum is -37, -69, -85 or
-93 dBm. On 0.3.2 the window maximums are:

| Maximum (dBm) | -97 | -96 | -95 | -94 | -93 | -92 |
|---|---|---|---|---|---|---|
| Windows | 56 | 9 | 12 | 8 | 6 | 2 |

-93 dBm is part of a smooth tail of real peaks. Tears would give all eight
torn values at equal rates, and 6 of 8 torn windows on one value is not
that. With -93 dBm left out of the rule ("strict rule"):

| Firmware | Floor -101.5 to -99.5 dBm | Other floors |
|---|---|---|
| 0.3.1 | 9 of 67 (13.4%) | 3 of 4 |
| 0.3.2 | 1 of 865 (0.1%) | 1 of 107 |
| 0.3.3 | 9 of 178 (5.1%) | 1 of 298 |

### 5. Longer gaps do lower agreement

Two clean reads in a row agree less often as the gap grows (0.3.3 traces,
reads after the stable read):

| Gap (µs) | 98-120 | 120-140 | 140-160 | 160-180 | 180-205 |
|---|---|---|---|---|---|
| Agree | 47.2% | 43.8% | 40.9% | 40.4% | 40.7% |

This supports the plan's reason for the rise in skipped samples on 0.3.3.

## What the audit did not confirm

- **The 0.3.1 spacing.** No 0.3.1 run logged read times. The 96.4 µs figure
  is what the model needs to give 13%. It means `micros()` plus the store
  cost about 0.5 µs per read, which is plausible but not measured.
- **A half-period minority.** About 7% of torn pairs on 0.3.3 (18 of 240)
  sit 3 to 4 µs off the 8 µs clock. The audit has no explanation. They may
  cause the few leaks 0.3.2 still has.
- **The datasheet.** The audit did not check the SX1278 datasheet for the
  RSSI update rate. The 8.00 µs comes from the data alone.
- **Skipped samples.** The simulation uses a register with two values. It
  gives 1.4 to 1.6% skipped samples for every spacing and does not show the
  rise to 4.5% on 0.3.3.
- **Which torn value leaks.** On 0.3.0 four of five leaks were -116 dBm, and
  on 0.3.1 five of thirteen were -108 dBm. The model predicts equal rates.
  The counts are small, and the audit did not pursue it.
- **Firmware 0.3.4.** The model predicts it behaves like 0.3.2. No board
  run existed at audit time.

## What this means for the firmware

These are inputs for a decision, not decisions.

- The 0.3.4 read path is sound today. Its margin is small: a change that
  shortens one read by 0.4 µs or more brings leaks back. A new compiler,
  a new RadioLib version, or a CPU clock change can do that.
- The comment on `RadioRssiSource::read()` says "Why 97 us works is not
  known". That is now known and the comment can say so.
- A spacing 2 or 6 µs past a multiple of 8 (98 or 102 µs) is furthest from
  both the main clock and the half-period minority.
- The firmware can report its own read spacing (for example the mean gap in
  each noise record), so a drift toward 96.0 µs shows up in the data.
- The window rule for "torn" should not count a -93 dBm maximum on its own.
- The register clock follows the LoRa bandwidth if finding 1 is right. A
  different bandwidth then needs a different safe spacing.

Board tests that would settle the open points:

1. Time a burst of reads with `micros()` only before and after the burst.
   That gives the 0.3.1 spacing. The model predicts about 96.4 µs.
2. Set the spacing to 96.0 µs on purpose. The model predicts torn windows
   near 40% with the floor near -100 dBm.
3. Set the spacing to 98.0 µs. The model predicts no leaks.
4. Run at 250 kHz bandwidth. The tear period should drop to 4 µs.

## Other notes

- The log has one packet, at 14:38:21 UTC (SM4JDW-9, CRC failed, SNR
  -21 dB). The plan still says no packet has been received.
- The plan counts 907 windows for 0.3.2 to 13:18 UTC. The audit counts 972
  to the restart at 13:28:58 UTC.

## How to reproduce

Inputs: `rssi_trace` and `noise` records in `logs/2026-11-13.jsonl`.

Firmware by `host_utc` time of day:

| Firmware | Noise windows | Traces |
|---|---|---|
| 0.3.1 | 10:32:00 to 10:44:30 (71) | none |
| 0.3.2 | 10:46:30 to 13:29:00 (972) | 10:47:05 to 11:11:31 (493) |
| 0.3.3 | 13:32:30 to 14:54:00 (485) | 13:32:48 to 14:33:17 (478) |

Rules:

- A torn read is one of -164, -132, -116, -108, -93, -85, -69, -37 dBm.
- Leave out traces with more than 8 torn reads. Those hold a real signal.
- Pair every two torn reads in the same trace, at any distance.
- The agreement table uses only reads after index `stable_reads`, which the
  trace trigger did not pick.

### Appendix A: period search

```python
import json, cmath, math

LOG = 'logs/2026-11-13.jsonl'
K = {-164: 2, -132: 3, -116: 4, -108: 5, -37: 2, -69: 3, -85: 4, -93: 5}

def tear_distances(start, end):
    """D in microseconds for every pair of torn reads in one trace."""
    out = []
    for line in open(LOG):
        try:
            r = json.loads(line)
        except ValueError:
            continue
        if r.get('type') != 'rssi_trace' or not start <= r['host_utc'][11:19] < end:
            continue
        d = [int(x) for x in r['reads_dbm']]
        t = r['t_us']
        torn = [i for i, x in enumerate(d) if x in K]
        if len(torn) > 8:
            continue
        for a, i in enumerate(torn):
            for j in torn[a + 1:]:
                out.append(t[j] - t[i] + 0.5 * (K[d[j]] - K[d[i]]))
    return out

def power(distances, period):
    s = sum(cmath.exp(2j * math.pi * x / period) for x in distances)
    return abs(s) ** 2 / len(distances)

D = tear_distances('13:32:00', '14:37:00')   # firmware 0.3.3
for T in (4, 8, 16, 32):
    print(T, round(power(D, T), 1))          # 8 gives 142.0, 16 gives 1.2
```

### Appendix B: simulation

```python
"""Model: the RSSI register changes only on an 8 us clock. SPI at 2 MHz shifts
the data byte out in 8 bits of 0.5 us. The register flips between 0x3F and
0x40 with chance p per clock tick. readStable takes the first value that two
reads in a row agree on (at most 8 reads). A window is 94 samples."""
import numpy as np

rng = np.random.default_rng(7)
TICK = 8.0; BIT = 0.5; PCROSS = 0.124; MAXREADS = 8; SAMPLES = 94
FLAG = np.array([0x00, 0x20, 0x30, 0x38, 0x7F, 0x5F, 0x4F, 0x47])
BITS = np.array([[(v >> (7 - m)) & 1 for m in range(8)] for v in (0x3F, 0x40)])

def run(N, spacing=None, jitter=None):
    if jitter is None:
        gaps = np.full((MAXREADS - 1, N), spacing)
    else:
        gaps = jitter[0] + rng.integers(0, jitter[1] + 1, size=(MAXREADS - 1, N)).astype(float)
    start = np.vstack([np.zeros((1, N)), np.cumsum(gaps, axis=0)])
    phase = rng.random(N) * TICK
    nt = int((start.max() + 8) / TICK) + 3
    flips = rng.random((nt, N)) < PCROSS
    st0 = rng.random(N) < 0.5
    state = (np.cumsum(flips, axis=0) + st0) % 2
    state = np.vstack([st0[None, :].astype(int), state])
    cols = np.arange(N)
    vals = np.zeros((MAXREADS, N), dtype=int)
    for j in range(MAXREADS):
        v = np.zeros(N, dtype=int)
        for m in range(8):
            t = start[j] + (m + 0.5) * BIT
            n = np.floor((t - phase) / TICK).astype(int) + 1
            v = (v << 1) | BITS[state[n, cols], m]
        vals[j] = v
    done = np.zeros(N, bool)
    out = np.full(N, -1)
    for j in range(1, MAXREADS):
        hit = (~done) & (vals[j] == vals[j - 1])
        out[hit] = vals[j][hit]
        done |= hit
    leak = np.isin(out, FLAG).mean()
    return np.isin(vals[1], FLAG).mean(), 1 - (1 - leak) ** SAMPLES, (~done).mean()

for name, kw in (('96.00', dict(spacing=96.0)), ('96.40', dict(spacing=96.4)),
                 ('96.87', dict(spacing=96.87)), ('random', dict(jitter=(98, 100)))):
    torn_reads, torn_windows, skipped = run(400000, **kw)
    print(name, round(torn_reads * 100, 2), round(torn_windows * 100, 2), round(skipped * 100, 2))
```
