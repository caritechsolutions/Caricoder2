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

Two layers deal with a source clock that does not run on smoothly. The
demux decides which PCRs to believe. The engine decides whether the clock
they describe is still the one the output is locked to.

**Which PCRs are believed (demux, live and file).** A PCR is *unusual* if
it does not follow on from the one before: it reads earlier, more than a
second later, or the stream flags a discontinuity. It is also unusual if
it merely comes after a gap much longer than this stream has been leaving
between PCRs (three times the longest recent gap, and at least 200 ms).

An unusual PCR is not acted on. A single damaged packet can carry any
value, and believing it would time real content seconds from where it
belongs. It is set aside as a candidate and the next PCR decides:

- it follows on, in the usual way, from the PCR before the candidate: the
  candidate was damage. It is forgotten and counted (`pcr_glitches`);
- it follows on from the candidate: the candidate was true. If it was a
  break, the break is placed at the candidate's packet. What was complete
  before that packet is timed on the old clock, what began after it on the
  new one, and a PES packet or section that straddles it is dropped;
- neither: it becomes the candidate. After three in a row with no two
  that agree, the latest is taken as it is.

A damaged PCR that reads only a little ahead (under 200 ms, or under
three PCR gaps on a stream with sparse PCRs) does not stand out when it
arrives and is taken as ordinary. If the next PCR then reads behind it
but follows on from the one before, that gives it away: it is taken out
of the history again and counted. The packets timed from it in between
leave late by up to the size of the error.

Waiting for the next PCR costs one PCR interval of delay, and only for
the content around an unusual PCR.

What this cannot catch: two or more damaged PCRs in a row that agree with
each other. They look exactly like a real change of clock.

**Is it still the same clock (engine, live).** The PLL knows where the
source clock should be right now. A believed PCR within 200 ms of that is
normal running. One further off opens a question, because two different
things look the same at first:

- data that was held up and is now being delivered faster than real time.
  The error shrinks as it catches up, and nothing should change. (A
  backlog caught up at less than about 1.2 times real time shrinks too
  slowly to be seen as moving, and is taken for a step: a known limit);
- a source that restarted on a new clock, stalled and stayed behind, or
  skipped ahead. The error stays where it is.

Only time tells them apart. While the question is open the outliers are
kept away from the PLL, and content is handled like this:

- if the clock read *ahead*, whatever was complete before its first new
  reading belongs to the old clock and is sent as usual;
- everything else is held to one side, not yet stamped for either clock,
  for at most 600 ms.

The question closes in one of these ways:

- the error comes back inside 200 ms: same clock. What was held is sent
  unchanged;
- the error holds still (within 50 ms) for 300 ms and three PCRs: the
  clock has stepped. `ts_offset` moves by exactly the difference, and what
  was held is sent stamped for the new clock. The new content lands on the
  existing output time line and the output PCR never steps;
- the error moves by more than 50 ms: probably a backlog draining. The
  wait starts again, and what was held is sent on the clock we know, but
  marked *tentative*;
- 600 ms pass with content still held (PCRs very far apart, or none): the
  same. The output thread enforces this limit too, so it holds even when
  the input has stopped altogether.

*Tentative* is a second chance for the case where that guess was wrong: a
real step whose error moved at first, or whose PCRs are too far apart to
confirm within 600 ms. When the step is then confirmed, every tentative
unit that is still waiting in the mux is moved onto the new clock
(`mux_restamp_tentative`). When the question closes the other way, the
marks are simply cleared. Its reach is limited. A unit stamped for the
wrong clock is only still waiting if that put it in the future, and by no
more than the 15 s the mux will queue ahead. So it saves content after a
step *forward* of up to 15 s. After a step backward the wrongly stamped
units are overdue at once and are sent or discarded as late before the
step is confirmed, which is what happened to all such content before
this mechanism existed.

In a live run the demux reports each PCR before it delivers the content
that arrived with it (`demux_set_pcr_first`), so the judgement on a PCR is
made before that content has to be stamped.

**A step, and then the PCR goes.** If the PCR stops while the question is
open, the stand-in clock takes over (next section). The time it gives is
the engine's, not that of the PCRs last seen, so the engine tells the
demux that it doubts them, and the demux drops its PCR history and
carries on along the engine's line. Units timed that way are marked (`wall`): their send
times are right as they are and are never moved, whatever is decided
about the clock later. Older content of the same streams that is still
held, or queued as tentative, is sent at once ahead of them, since it can
no longer be placed any better and would otherwise block them.

What all this costs, measured by the tests:

- a step with PCRs every 30 ms (restart on a clock two hours away, a
  stall of 2.6 s that stays late, a skip of 2 s) is confirmed in about
  300 ms, inside the default 500 ms delay: nothing is dropped, nothing
  leaves late, no packet carries the wrong clock's timestamps. With
  `--delay` under about 350 ms the held content leaves slightly late;
- the same with a PCR only every 240 ms takes about half a second to
  confirm: nothing is dropped, and a few packets leave up to 0.1 s late;
- a source that skips 3 s ahead and then delivers faster than real time
  for half a second: nothing is dropped, and about a second of content
  leaves late, by up to 0.7 s. Part of that is unavoidable: the source
  itself sent it later than the clock it settles on says it should have;
- a step of 3 s either way with the PCR lost 150 ms later while audio
  continues: all the audio is delivered, with no gap longer than 0.2 s.
  Its timestamps stay on the old clock until the PCR is back. (After a
  step too large for the timestamp range check, 5 s back or 30 s ahead,
  that audio is discarded instead: nothing sensible can be done with
  timestamps that far from the only clock there is);
- holding has a price when the question turns out to be a backlog: that
  content was late already and is now up to 0.3 s later (0.6 s with
  sparse PCRs). If it was more than about 0.7 s late to begin with, some
  of it passes the one second limit and is dropped rather than sent late;
- in all cases, a PES packet that arrives after the source's clock has
  stepped but before its next PCR cannot be told from old-clock content.
  It is sent with timestamps that are off by the step. That is at most one
  PCR interval of content.

This was first run against a real satellite feed for 34 hours. The source
stepped eight times in that period, in three shapes that are now each a
test: a stall that stays late, a skip forward, and a full restart.

**File.** There is no wall clock, so a confirmed break of more than
100 ms is closed up at once: `ts_offset` moves by the difference.

### A stream that leaves the service half way through a packet

When the PMT drops a stream, or the service changes, the mux may be part
way through sending one of that stream's PES packets. Cutting it off
would put a truncated PES packet on the wire. The stream is instead kept
as an *orphan*: its unsent queue is dropped, the packet in flight is
finished, and then it is gone. The exception is a PID that is handed
straight to something else, a table or a stream of another kind. Two
things cannot share a continuity counter mid-packet, so there the packet
in flight is cut off.

### When the PCR goes missing

If data keeps arriving on a live input but its PCR stops, the PLL is still
running. Once data has been arriving steadily for 500 ms with no PCR, the
engine tells the demux the time every 100 ms in the PCR's place, and the
streams that remain keep flowing on the same time line. A PCR that has
been seen but not yet believed (a candidate, above) is given five of
these intervals to be borne out before the stand-in takes over. "Steadily" is part
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
| Source restarts, stalls and stays late, or skips ahead | One discontinuity absorbed, see above |
| One PCR with a wrong value | Not believed; counted in `pcr_glitches` |
| Two or more wrong PCRs in a row that agree | Believed: taken for a change of clock, and again when the true values return |
| Stream removed from the PMT mid-packet | The PES packet in flight is finished, the rest of its queue dropped (cut off if the PID is reused at once) |

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
| H.264 / H.265 decode on CPU | libavcodec (decided, see below) |
| H.264 / H.265 on NVIDIA | NVIDIA Video Codec SDK (NVDEC, NVENC) |
| H.264 / H.265 on Intel | Intel VPL |
| AAC | fdk-aac |
| MP2 / MP3 | twolame, LAME, mpg123 |
| AC-3 decode | liba52 |

Each stream will take `auto`, `nvidia` or `cpu` for its decoder. `auto`
tries the GPUs in turn and then the CPU.

**CPU decoding uses libavcodec** (decided 11 October 2026). No standalone
H.264 decoder was found fit for broadcast: OpenH264's own documentation
limits its decoder to Constrained Baseline, single-threaded. libavcodec is
the decoder inside FFmpeg, and it is also what GStreamer's software
decoding was all along (`gst-libav`). The terms of its use here are narrow:
the decoder library only, called directly, as one optional backend. The
demux, mux, clock and timestamps stay our own; nothing of FFmpeg's
pipeline, formats or tools is used; and a build without it is still a
complete GPU transcoder.

**NVIDIA target** (facts for the first deployment box, driver 535.247.01,
two Tesla P40): write against Video Codec SDK 12.1, which needs driver
530.41.03 or newer; 12.2 needs 550.54.14. A P40 has two encoder engines
and unrestricted sessions, encodes H.264 and H.265 including 10-bit, and
has no H.265 B-frames and no AV1.

The frame-level interface (packets in, frames out, and the reverse) will be
written with the first backend that needs it. Guessing it now would mean
rewriting it then.

### Licensing, to check before anything is distributed

x264 and x265 are GPL. fdk-aac's licence is not GPL-compatible. Running
both on your own servers is not distribution and raises no issue. Shipping
a binary that links both to a third party would. This is not legal advice;
it is a flag to raise with whoever advises you before a binary leaves your
hands.
