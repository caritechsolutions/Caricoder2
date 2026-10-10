# caricoder3: how it works

This explains the decisions behind the code so that the next stages can be
built on them without re-deriving anything. File names refer to `src/`.

## The shape

```
            input thread                         output thread
 UDP/RTP ──► demux ──► units ──────────────────► mux ──► UDP
 (or file)   demux.c   (PES packets, sections,   mux.c   (or file)
                        each with a send window)
                 │                                 ▲
                 └── PCR ──► clock (PLL) ──────────┘
                             clock.c, engine.c
```

Stage 2 and 3 put a decoder and an encoder on the arrow in the middle for
the streams that are transcoded. Copied streams keep using the arrow as it
is. Nothing else moves.

| File | Job |
|---|---|
| `cc3.h` | Time base, TS header helpers, the `es_unit` that travels between stages |
| `psi.c` | Section reassembly; PAT, PMT, SDT parsing and building |
| `demux.c` | Packets in, timed units out |
| `mux.c` | Timed units in, packets out |
| `clock.c` | The PLL that locks to the source clock |
| `engine.c` | Glue: time lines, discontinuities, live and file running |
| `udp.c`, `main.c` | Sockets, threads, options, statistics, systemd |
| `codec.c` | Registry for codec backends (empty in stage 1) |

## One time base

Inside the program all time is 27 MHz ticks in a signed 64-bit integer that
never wraps. The 33-bit PCR, PTS and DTS fields are unwrapped as they come
in and wrapped again as they go out. Nothing in between knows about the
26.5-hour rollover.

## Timing by position, not by arrival

Each incoming packet gets a running index. A PCR packet pins its index to a
clock value. Every other packet's time is interpolated between the two
PCRs around it, by position.

The machine's own clock is not involved, which has three consequences:

1. Network jitter cannot disturb the timing of the content.
2. The demux and mux together are a pure function of the input bytes. A
   file run gives the same output bits every time, on any machine. The
   tests rely on this.
3. A unit cannot be timed until the PCR after it arrives (tens of
   milliseconds). The delay buffer absorbs that.

If PCRs stop while data keeps coming, the last known rate is carried
forward so the data still gets out (`pcr_missing` counts this).

## Units and send windows

The demux emits *units*: one complete PES packet, or one complete section.
Each carries a send window `[t0, t1)`, the span over which it arrived.

The mux spreads a unit's packets evenly across its window. A large frame
that took 200 ms to arrive therefore takes 200 ms to leave, so the output
has the same shape as the input instead of bursts.

This is also the contract for encoders later: an encoder hands the mux an
access unit with a window derived from its rate control, and the mux needs
no special case for it.

A copied PES packet is carried with its original header bytes. If the time
line has been shifted (see below) the 33 timestamp bits are patched in
place; nothing else is touched.

## The mux: slots

The output is a train of packet slots of fixed duration:

```
STC(i) = STC(0) + i × 188 × 8 × 27,000,000 / muxrate
```

kept with an integer remainder, so it is exact forever. A PCR written into
slot `i` is `STC(i)`. PCR accuracy against byte position is therefore
correct by construction, not something that has to be achieved by careful
pacing.

Each slot is filled by the first of these that applies:

1. New tables, at start-up or after a service change (so no receiver meets
   a stream before the PMT that announces it).
2. A PCR that is due. From 30 ms it rides on the next packet of the PCR
   stream at no cost; only if that stream stays silent until 37.5 ms is a
   packet spent on a PCR alone.
3. A table packet.
4. The queued packet with the earliest send time already reached.
5. A null packet.

VBR is the same machine with one difference: null packets are not handed
to the output. The slot clock still runs, so PCR values stay consistent.

Continuity counters are kept per output PID, not per stream or table, and
are stamped at the moment a packet is sent. They therefore stay unbroken
when a PID changes hands (a PMT moving onto a PID that had carried video)
and when two things share one PID (the PCR on the PMT's PID).

## Two time lines and one offset

- *Input time*: the source's clock, as carried in its PCRs.
- *Output time*: input time plus `ts_offset`.

`ts_offset` starts at zero, so in normal running the output carries the
very same PTS and DTS values as the input. The mux's "now" is

```
live : pll(wall clock) − delay
file : latest input PCR + ts_offset − delay
```

### Discontinuities

The demux reports a PCR that does not follow on from the last one. What
happens next depends on the mode.

**Live.** The PLL knows where the source clock should be right now. A PCR
within 200 ms of that is normal running. One further off is not acted on
at once, because two different things look the same at first:

- data that was held up and is now arriving in a burst. It catches up with
  the wall clock within the burst, and nothing should change;
- a source that restarted on a new clock. It stays off.

Only time tells them apart. So the outliers are kept away from the PLL, and
if the PCR is still off after 300 ms and five samples, it is a new clock:
`ts_offset` moves by exactly the difference, the new content lands on the
existing output time line, and what was queued during those 300 ms with the
old offset is discarded. The output PCR never steps.

**File.** There is no wall clock, so any break is closed up at once the
same way.

Partly assembled PES packets are discarded at a break.

### When the PCR goes missing

If data keeps arriving on a live input but its PCR stops, the PLL is still
running. Once data has been arriving steadily for 500 ms with no PCR, the
engine tells the demux the time every 100 ms in the PCR's place, and the
streams that remain keep flowing on the same time line. "Steadily" is part
of the rule: right after an outage the first datagrams are old data whose
own PCR is about to follow, and the wall clock says nothing true about
them.

A file run has no clock to fall back on. There the last measured rate is
carried forward by packet count, which is exact for a CBR input and an
approximation for a VBR one.

### When this process stalls

If the output thread finds itself more than half a second behind (the
process was stopped or starved, or the virtual machine was paused), the
missed time is not sent late in a burst. The output clock steps forward to
now, the next PCR carries the discontinuity indicator so receivers know,
and content that is still current goes out on time. Up to half a second is
simply caught up.

## The clock

A second-order loop (proportional + integral) fed by PCR arrival times.
It maps the machine's monotonic clock onto the source's 27 MHz clock. The
output is paced from that mapping, so the queue between input and output
neither fills nor empties however far the two crystals drift apart.

The loop starts wide to pull in quickly and narrows after 150 samples.
When the input stops, it free-runs at the last measured rate.

Jitter in the PCR arrivals moves only the *pacing* of the output slightly.
It cannot reach the PCR *values*, which come from slot arithmetic.

## What happens when the input is bad

| Input | Response |
|---|---|
| Continuity error on a PID | The PES packet or section being assembled on that PID is discarded and counted. The next one starts clean |
| Same counter twice | A true duplicate (identical packet) is dropped, as the standard allows. Different content means 15 packets were lost: treated as an error |
| Transport error flag | Packet ignored entirely, including its PID |
| Lost byte alignment (file) | A packet is only accepted once the next one is seen to start in the right place; then a three-packet sync hunt |
| PES without start code, or with a broken header | Discarded, counted |
| Timestamps nowhere near the PCR | Discarded, counted |
| Table with a bad CRC, or a good CRC but malformed inside | Ignored and counted; the last good table stays in force |
| PCR stops, data continues | Timed from the recovered clock (live) or by packet count (file) |
| Data delayed, then a burst | Sent as it comes; the time line is not moved |
| PMT changes | Streams that keep their PID carry on untouched; new ones are added, old ones flushed; the output PMT version moves |
| More content than `--muxrate` | Whole units are dropped once they are a second late, never partial ones; counted in `dropped_units` |
| Input stops | Output continues with tables, PCR and nulls |
| Source restarts | One discontinuity absorbed, see above |

The rule throughout: the output is always a well-formed transport stream,
and anything thrown away is thrown away whole and shows up in a counter.

## Threads

Three in live running, and one mutex around the mux and the clock.

- *Input* blocks in `recvmmsg()`, runs the demux, and takes the mutex only
  to queue a finished unit or feed the PLL.
- *Output* wakes exactly when the packet that completes the next datagram
  is due (`clock_nanosleep` on an absolute time, timer slack set to 1 ns),
  runs the mux up to now, and sends.
- *Main* writes statistics once a second and pets the systemd watchdog if
  the other two are alive.

A file run uses one thread and no clock.

## Where codecs will plug in

`codec.h` describes a backend: a name, the codecs it handles, decoder or
encoder, a priority, and a `probe()` that says whether its hardware, driver
and library are usable on this machine now. Backends are compiled in only
when their library is found at build time and probed at start-up. One
binary then runs on a box with an NVIDIA card, one with an Intel GPU, or
one with neither, and picks the best backend that works unless the channel
names one.

Planned libraries, each called directly:

| Job | Library |
|---|---|
| H.264 / H.265 encode on CPU | x264, x265 |
| H.264 / H.265 on NVIDIA | NVIDIA Video Codec SDK (NVDEC, NVENC) |
| H.264 / H.265 on Intel | Intel VPL |
| AAC | fdk-aac |
| MP2 / MP3 | twolame, LAME, mpg123 |
| AC-3 decode | liba52 |

Still to be settled when stage 3 starts: the software H.264/H.265
*decoder* for boxes with no GPU. The usual standalone choices need to be
evaluated; this is the one place where the "no FFmpeg" rule has a real
cost, and the decision should be made on facts at that point.

The frame-level interface (packets in, frames out, and the reverse) will be
written with the first backend that needs it. Guessing it now would mean
rewriting it then.

### Licensing, to check before anything is distributed

x264 and x265 are GPL. fdk-aac's licence is not GPL-compatible. Running
both on your own servers is not distribution and raises no issue. Shipping
a binary that links both to a third party would. This is not legal advice;
it is a flag to raise with whoever advises you before a binary leaves your
hands.
