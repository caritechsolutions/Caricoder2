# caricoder3

A live MPEG-TS to MPEG-TS processor written from the ground up in C.
No GStreamer, no FFmpeg, no libraries beyond the C library.

This is the foundation for replacing Flussonic's transcoder. It lives in its
own folder and shares no code with the GStreamer-based CariTranscoder in the
rest of this repository.

## Where it stands: stage 1 of 5

| Stage | What | State |
|---|---|---|
| 1 | TS in → demux → remux → TS out, every stream copied | **this folder** |
| 2 | Audio transcode (AAC, MP2, MP3 in and out; AC-3 in) | not started |
| 3 | Video decode + encode: x264/x265, NVIDIA, Intel | not started |
| 4 | Scaling, deinterlace, several outputs from one decode | not started |
| 5 | Supervision, API, GUI, side-by-side run against Flussonic | not started |

Stage 1 contains no codec. It proves the parts every later stage stands on:
the demultiplexer, the multiplexer, the clock, and the behaviour when the
input misbehaves. MPEG-2 video is parked by decision.

## Build

```
cd caricoder3
make            # -> ./caricoder3        needs only gcc and make
make test       # builds the sanitizer version too and runs the test suite
sudo make install
```

## Use

One process handles one channel.

```
# Constant bit rate, e.g. towards a satellite multiplexer
caricoder3 -i udp://239.5.5.5:5000 -o udp://239.6.6.6:6000?ttl=8 --muxrate 5M

# Variable bit rate (no null packets): the default when --muxrate is absent
caricoder3 -i udp://239.5.5.5:5000 -o udp://239.6.6.6:6000

# One service out of a multi-program stream, on a named interface
caricoder3 -i "udp://224.100.1.5:49500?iface=eth1" -o udp://239.6.6.6:6000 \
           --program 1650 --muxrate 4M

# Rename the service on the way through
caricoder3 -i ... -o ... --service-name "Vacation" --service-provider "Coop Cable"

# File to file: as fast as the disk allows, same result every time
caricoder3 -i capture.ts -o out.ts --muxrate 4M
```

`caricoder3 --help` lists every option. The ones that matter most:

| Option | Meaning |
|---|---|
| `--muxrate RATE` | CBR output at this rate (`5M`, `4500k`). Must exceed the content rate by a few percent |
| `--vbr` | With `--muxrate`: VBR capped at that rate |
| `--delay MS` | Input-to-output buffer, default 500. See "Delay" below |
| `--program N` | Service to take. Default: the first one in the PAT |
| `--stats-file PATH` | JSON status, rewritten atomically once a second |
| `--rt-prio N` | Real-time priority for the output thread (needs `CAP_SYS_NICE`) |

Input URLs accept `udp://` and `rtp://`, a source for source-specific
multicast (`udp://10.0.0.1@232.1.1.1:5000`), and `?iface=`, `?buf=`.
RTP is detected per datagram, so a `udp://` URL works for an RTP source.
The receive socket binds to the group address itself, so two streams that
share a port number do not get mixed.

Output URLs accept `?ttl=` (default 8), `?iface=`, `?buf=`, and `?pkt=`
(TS packets per datagram, default 7).

### Under systemd

```ini
[Unit]
Description=caricoder3 channel %i
After=network-online.target

[Service]
Type=notify
ExecStart=/usr/local/bin/caricoder3 -i udp://239.5.5.5:5000 \
          -o udp://239.6.6.6:6000?ttl=8 --muxrate 5M \
          --stats-file /run/caricoder3/%i.json
RuntimeDirectory=caricoder3
RuntimeDirectoryPreserve=yes
Restart=always
RestartSec=1
WatchdogSec=10
AmbientCapabilities=CAP_SYS_NICE

[Install]
WantedBy=multi-user.target
```

The process reports `READY=1` once its threads are up and pets the watchdog
once a second, but only while both the input and the output thread are
really turning over. A hung process is restarted by systemd; a missing
input is not a hang and does not cause a restart.

## What it does to a stream

**Kept exactly:** every PES packet, byte for byte, including its header
and its PTS/DTS; every section of section-carried streams (SCTE-35, private
sections such as the RIST Part 7 marker PID); PIDs; stream types; all PMT
descriptors; the service's SDT descriptors; the choice of PCR PID; the
random-access flag on key frames.

**Rebuilt:** packetisation, continuity counters, PCR, PAT, PMT and SDT
(single service), null stuffing.

**Guarantees on the output**, whatever the input does:

- Continuity counters are always correct on every PID.
- CBR: each PCR equals its byte position to within one 27 MHz tick (37 ns).
  It is computed from the position, not measured, so it cannot drift.
- PCR every 30 ms nominal, 37.5 ms plus one packet slot at worst.
  PAT/PMT every 100 ms, SDT every 500 ms.
- A table's version number changes when, and only when, its content does.
- If the input stops, the output does not: tables, PCR and (in CBR) null
  packets keep flowing, and the service resumes on the same time line when
  the input returns.
- If the source restarts on a new clock, the output clock runs on unbroken
  and the new content is spliced onto it. PTS/DTS are shifted by one
  constant; nothing else in the PES packets changes.
- Input that is merely held up and then arrives in a burst is not mistaken
  for a new clock: the time line is left alone.
- If the PCR disappears while other streams keep arriving, they are timed
  from the clock already recovered and keep flowing.
- If the process itself is frozen for a while (a virtual machine pause, for
  example), it does not dump the missed time onto the wire afterwards. The
  output clock steps forward once, and the step is flagged in the stream.
- The 33-bit PCR/PTS rollover (every 26.5 hours) is passed without incident.
- A PES packet that lost data on the way in (continuity error) is dropped
  whole and counted, never forwarded damaged.
- A table that is malformed, even with a correct CRC, is ignored and
  counted; the last good one stays in force.

### Delay

Everything waits `--delay` milliseconds between input and output. That is
what allows a whole PES packet to be collected before it is sent and still
leave on schedule. The default of 500 ms reproduces the input's own pacing
exactly. A smaller value still gives a valid, complete stream (tested at
60 ms); large frames then leave in a burst instead of evenly. Stage 3 will
need more than 500 ms once decoding and encoding sit in the path.

## Statistics

`--stats-file` produces one JSON object:

- `state`: `waiting_for_input`, `waiting_for_clock` or `running`
- `input`: bit rate, age of the last packet, continuity errors on this
  service and on the rest of the multiplex, sync losses, kernel socket
  overflows, damaged PES dropped, bad tables, PCR jumps, PCR missing
- `service`: PIDs, stream types, how each stream is handled
- `clock`: locked or not, measured drift against the source in ppm, arrival
  jitter, time-line offset, number of discontinuities absorbed
- `output`: mode, bit rate, share of null packets, units dropped for lack
  of output rate, units that left after their own DTS, clock skips after a
  process freeze, queue depth, send errors

The same figures go to the log every 10 seconds (`--stats-interval`).

## Tests

```
make test                         # 42 tests, about four minutes
python3 tests/run_tests.py --list
python3 tests/run_tests.py live   # only the tests with "live" in the name
make fuzz && build/fuzz_engine -max_len=400000 build/corpus
```

The checker in `tests/` is a second, independent implementation of the
transport stream standard in Python. It shares nothing with the C code, so
agreement between the two is evidence rather than an echo. Where ffmpeg and
TSDuck are installed, the suite also has them decode and verify the output.

File tests run under AddressSanitizer and UndefinedBehaviorSanitizer. Live
tests run in real time over UDP on the loopback interface. The fuzzer
checks every output packet for a valid header, an unbroken continuity
counter and a forward-moving PCR, on top of the usual memory checks.

To check a stream of your own, on any machine with Python 3:

```
python3 tests/ts_check.py out.ts --cbr --against in.ts
```

## Known limits of stage 1

- IPv4 only.
- One service per process. A multi-program input is fine; the output is
  single-program.
- Scrambled streams are counted and not carried.
- EIT, NIT, TDT/TOT and other SI are not carried. PAT, PMT and SDT are.
- A file can only be written to a file, not played out to UDP.
- A service with no PCR cannot be processed.
- In a file run, if the PCR disappears while other streams continue, their
  timing is carried forward by packet count: exact for a CBR input,
  approximate for a VBR one. A live input does not have this limit.
- A burst that removes an exact multiple of 16 packets of one PID cannot be
  seen by a 4-bit continuity counter. That is a property of the transport
  stream, not of this program.
- On loss of input the output keeps running. Going silent after a timeout,
  so that a downstream failover switch can react, is not built yet.
- No PID or service-ID remapping.

See [docs/DESIGN.md](docs/DESIGN.md) for how it works inside.
