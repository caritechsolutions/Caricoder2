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

A stream that is transcoded leaves that arrow after it has been given its
place in time, goes through a decoder and an encoder, and joins the arrow
again as new units ("Transcoding" below). Copied streams use the arrow as
it is. Nothing else moves.

| File | Job |
|---|---|
| `cc3.h` | Time base, TS header helpers, the `es_unit` that travels between stages |
| `psi.c` | Section reassembly; PAT, PMT, SDT parsing and building |
| `demux.c` | Packets in, timed units out |
| `mux.c` | Timed units in, packets out |
| `clock.c` | The PLL that locks to the source clock |
| `engine.c` | Glue: time lines, discontinuities, live and file running |
| `udp.c`, `main.c` | Sockets, threads, options, statistics, systemd |
| `codec.h`, `codec.c` | The four interfaces a codec backend implements; the registry that picks one |
| `xcode.c` | One transcoded stream: its queue and thread, PES packets, option parsing |
| `xc_audio.c`, `aframe.c` | Audio: framing, sample counting, encoder feed |
| `xc_video.c` | Video: decoder to encoder, send times |
| `a_*.c`, `v_*.c`, `nv_*.c` | One file per codec library |

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

Three in live running, plus one for each transcoded stream, and one mutex
around the mux and the clock.

- *Input* blocks in `recvmmsg()`, runs the demux, and takes the mutex only
  to queue a finished unit or feed the PLL.
- *Output* wakes exactly when the packet that completes the next datagram
  is due (`clock_nanosleep` on an absolute time, timer slack set to 1 ns),
  runs the mux up to now, and sends.
- *Main* writes statistics once a second and pets the systemd watchdog if
  the other two are alive.
- *One per transcoded stream* takes that stream's units from a queue,
  decodes, encodes, and takes the mutex only to hand a finished unit to
  the mux. The codec libraries start threads of their own inside that.

A file run uses one thread and no clock.

## Transcoding

### Backends

`codec.h` holds four small interfaces: audio decoder, audio encoder, video
decoder, video encoder. A backend implements one of them for one library
and says which codecs it handles, whether it runs on the CPU or on an
NVIDIA card, and how good it is (priority). Each lives in a file of its
own and is compiled only if the Makefile found its library. At run time
`codec_find()` returns the best one that is usable right now; a backend
with a `probe()` (the NVIDIA ones) can say "not on this machine". One
binary therefore runs on a box with a card and on one without.

| Job | Library | File |
|---|---|---|
| AAC decode, AAC-LC encode | fdk-aac | `a_fdk.c` |
| MPEG audio layers I-III decode | mpg123 | `a_mpg123.c` |
| AC-3 decode | liba52 | `a_a52.c` |
| MP2 encode | twolame | `a_twolame.c` |
| MP3 encode | LAME | `a_lame.c` |
| H.264 / H.265 decode, CPU | libavcodec | `v_avcodec.c` |
| H.264 encode, CPU | x264 | `v_x264.c` |
| H.265 encode, CPU | x265 | `v_x265.c` |
| H.264 / H.265 decode, NVIDIA | NVDEC (libnvcuvid) | `v_nvdec.c` |
| H.264 / H.265 encode, NVIDIA | NVENC (libnvidia-encode) | `v_nvenc.c` |

**CPU decoding uses libavcodec** (decided 11 October 2026). No standalone
H.264 decoder was found fit for broadcast: OpenH264's own documentation
limits its decoder to Constrained Baseline, single-threaded. libavcodec is
the decoder inside FFmpeg, and it is also what GStreamer's software
decoding was all along (`gst-libav`). The terms of its use here are narrow:
the decoder library only, called directly, as one optional backend. The
demux, mux, clock and timestamps stay our own; nothing of FFmpeg's
pipeline, formats or tools is used; and a build without it is still a
complete GPU transcoder.

### Where a transcoded stream leaves the pipeline and rejoins it

The engine gives every unit from the demux its place on the output time
line (the offset, and everything in "Discontinuities"). Only then does it
look at the PID: a copied stream's unit goes to the mux; a transcoded
stream's unit goes to that stream's `xcode`. So a source clock step is
dealt with once, in one place, and the codecs only ever see time that runs
on. What an `xcode` produces comes back as ordinary units with send
windows and goes into the mux like any other.

In a live run each `xcode` has a thread and a queue, so neither the input
nor the output thread ever waits on a codec; the rest of the service is
never held up by one. In a file run the work is done in the caller's
thread, with no queue.

**Falling behind.** What comes out of a transcoder after its send time
goes out late, and is dropped by the mux once it is later than the mux
allows; so a codec that is behind does all the work for little or
nothing. The thread watches for that (`check_late` in `xcode.c`). Every
3 s that the output has been late without a break, it compares how late
the output is now with the worst since it last looked:

- less, by 0.3 s or more: the codec is working off a backlog (from its
  start, or a stall) and is left to it;
- not less, with more than half a second of input piled up: the codec is
  slower than the stream and will never catch up by itself. The queue is
  thrown away (counted as `input_dropped`), a video decoder is reset and
  its input skipped up to the next key frame (for 5 s at most: a stream
  whose key frames are not recognised is better decoded with errors than
  not at all), and a warning says so;
- not less, with nothing piled up: the codec keeps up, but `--delay` is
  shorter than what the codecs hold back. That is reported every 10 s and
  nothing is thrown away, since it would not help.

A 48 MB cap on the queue remains as a guard on memory.

When the PMT changes, the set of transcoders is brought in line: one whose
stream and settings are unchanged is kept running; the others are
finished (the queue worked off, the codecs emptied, so the end of the old
stream goes out), stopped and replaced, outside the lock, since that means
waiting for a thread. The wait for the queue is limited to 0.3 s, because
it is the input thread that waits: a codec not through by then was behind
anyway, and what it held is given up. Emptying the encoder afterwards
takes what it takes (tens of milliseconds at the default presets). A new transcoder taking over a PID is told the last
decode time its predecessor gave out and when that unit will have been
sent, so decode times on the PID only move forward and the new stream is
sent after the old one's end, not on top of it.

### Audio: time by counting samples

A PES packet carries a run of audio bytes that need not start or end on a
frame boundary, and one time stamp, for the first frame that starts in it.
`aframe.c` cuts that into whole frames, each with its time, and finds its
way back after damage (a first frame is believed only if another follows
it; AC-3 frames are CRC-checked, because liba52 does not look).

From there on, time is kept by counting samples, not by trusting each
stamp on its own. The first block of samples fixes an origin. Each later
block is expected where the samples before it end:

- its stamp agrees to within 10 ms: nothing changes, so rounding in the
  source cannot make the output jitter;
- its stamp is later (audio is missing, typically a lost packet): the hole
  is filled with silence, up to one second, and the output frames keep
  their spacing;
- its stamp is earlier: the overlap is cut;
- more than a second either way: a new origin. That is what a source
  clock step looks like from here.

Encoder frames are timed from the same count: frame k begins at sample
k x frame size, less the delay the codec itself adds, so that what a
receiver plays at a given moment is what the source had at that moment.
Those delays are 1024 + 576 samples for AAC-LC (reported by the library),
481 for MP2 and 576 + 529 for MP3. They are not taken on trust: the tests
decode both the original and the result with a reference decoder and
slide the two against each other, and every pairing comes out within
0.02 ms.

HE-AAC needs one more rule. The library reports that its HE-AAC output
lags by the delay of its SBR stage plus its own post-processing. Of that,
962 samples are the SBR delay every HE-AAC decoder has, and receivers play
such a stream with that lag built in. So only the rest is taken out.

**Output packets and the decoder's buffer.** A standard decoder has 3584
bytes for an audio stream. What sits in it is the audio between "has
arrived" and "is played". A PES packet is sent over as long as it plays
and has all arrived a lead before its first sample is due, so the buffer
holds (packet + lead) of audio at the bit rate. Both are sized per
encoder to keep that under 3000 bytes, and under 200 ms at low bit rates:
AAC at 128 kb/s gets four frames to a packet and about 100 ms of lead.
The lead is also what the audio has in hand when a CBR mux is busy with
something else, so it is never less than 75 ms; packets are shortened
first, down to one frame. For MP2 at 320 and 384 kb/s, 75 ms is by itself
more than the buffer holds, and the lead is kept all the same: a packet
that comes late is heard, a buffer over its nominal size usually is not.
Measured in a CBR mux beside new video or with room to spare: 1760 to
3384 bytes across five codings and rates, 4664 for MP2 at 384 kb/s, and
never less than 75 ms early. Beside a copied video stream in a mux with
under a tenth to spare the lead is eaten into (43 ms was measured): see
"Known limits" in the README.

### Video: pictures keep their time

Pictures go from decoder to encoder with their time stamps untouched, and
the encoder is told to use them (variable frame rate input), so a source
that skips or stutters is coded for the time it really takes. Size, frame
rate, pixel shape, field order and colour description are passed on.
They are compared on every picture, and a change of size, pixel shape or
frame rate reopens the encoder at that picture. Field or frame is a flag
on each picture and interlaced streams carry progressive pictures among
the others, so one interlaced picture makes the stream interlaced at
once, and only 500 progressive pictures in a row make it progressive
again. Colour description and field order must stay changed for 10
pictures: libavcodec, working on several pictures at once, reports an
SEI's transfer function on some pictures and the sequence's on others,
which reopened the encoder 14 times in a second before this rule.
H.264 is encoded at 8 bits whatever comes in (10-bit pictures are
converted, rounded); H.265 keeps the depth of the source.

libavcodec reads slightly past the end of what it is given and requires
zeroed room there. A PES packet has none, so its payload is copied to a
buffer that does. (AddressSanitizer found this.)

**When each access unit is sent.** An encoder that holds to a buffer of B
seconds at its bit rate R promises this: if its output is delivered at R,
starting B before the first picture is due, every picture is complete in
the receiver before it is needed. So that is how the output is delivered.
Each access unit takes (its size / R) to send, one follows the other, and
none starts earlier than (its decode time - B - one picture). B is
`--vbv`, 1 s by default. A large key frame is thus spread over the time
it takes at R instead of being pushed out in one picture's time, and the
audio beside it is not held up.

The pace is not exactly R. It counts the encoder's bytes only (not the
PES header added here) and runs 1.5 % above R. Being behind the "not
sooner than" line is normal (a large key frame takes the schedule a good
part of a buffer behind, and the encoder has allowed for it), so nothing
more is done up to three tenths of a buffer behind. Past that the pace
rises with the square of the excess: half a buffer behind, 16 % more; a
whole buffer, three times R. The first version paced everything at exactly R,
header included, and so ran a fraction slower than the encoder produced:
over a minute of hard-to-code video the pictures slid from a second early
to barely in time. Now an encoder that keeps to R is sent at R but for a
short while after its largest pictures, and one that runs over (x265, by
6 % in the test) settles some way behind the line and stays there. (With
the rise starting at half a buffer, x265 was left with 0.11 s at its
worst; starting at three tenths costs x264 a higher short-term peak and
gives x265 three times the margin.) As a last resort a unit is always
delivered by half a picture before its decode time.

Measured on the wire at 500 kb/s with easy pictures: every picture
complete 0.77 to 1.02 s early, the receiver never holding more than 67 kB
(the buffer is 62.5 kB plus a picture), never more than 600 kb/s over any
400 ms, in VBR and in CBR. With four minutes of noise that keeps the
encoder at its full rate throughout: x264 at least 0.39 s early (median
0.88 s), the receiver holding 74 kB at most (the largest picture there is
32 kB), 1.38 times the bit rate at most over 0.4 s and 1.24 over 1 s,
transport packets and all; x265 at 200 kb/s, which runs over its rate, at
least 0.36 s early (median 0.61 s). In a file run the output
clock starts B earlier than the first unit, since the first new pictures
are sent before anything in the input was (but not before time zero).

**Delay.** An access unit can only be sent once it exists, and it exists
only after the decoder and the encoder have had the pictures they hold
back and done their work. Left alone, x264 starts one and a half threads
per processor and libavcodec one per processor, and each thread is one
more picture held back: 17 pictures on a 2-processor machine, 48 on a
32-processor one, which is more than the delay allows. So the threads are
limited per stream (4 to decode, 6 for x264; x265 limits itself to 6),
and what is held back no longer depends on the machine. The engine's
delay has to cover that plus B. It is fixed when the program starts,
because changing it later would be a step in the output clock; so it
defaults to 3 s for video (B above 1 s is added on top; a delay under
B + 0.5 s is refused, since nearly all video would come too late) and
0.8 s for audio. On every unit that comes back from a transcoder the
engine measures how long it still had before it was due (`slack`). At
1080p in a file run the video had 1.6 to 1.8 s to spare with 2 processors
and 1.5 s with 64; in the live test, 1.8 s.

### NVIDIA

First run on a card on 2026-10-14 (see the README); the key-frame label
in field mode was the one thing the card did differently from the
headers (see `v_nvenc.c`). What the code does:

- `nv_common.c` opens the driver's three libraries at run time and keeps
  one CUDA context per card for the whole process. Everything on a card
  shares it, which is what lets a decoded picture be handed to the encoder
  without leaving the card.
- `v_nvdec.c` feeds the card's own parser, which calls back when it knows
  the format (the decoder is created then), when a picture is to be
  decoded, and when one is ready to show. Shown pictures are queued and
  handed out one at a time, mapped as one NV12 (or 16-bit) surface.
  Asked for 8-bit pictures from 10-bit video, the card converts.
- `v_nvenc.c` keeps a pool of eight surfaces registered with the encoder.
  A picture from the card is copied into one, card to card; a picture
  from the CPU decoder is interleaved and uploaded. H.264 is coded with
  two B-frames between references (frameIntervalP 3), H.265 without
  (none on Pascal). With B-frames the card answers "need more input"
  until a reference is coded, then the group comes out at once in decode
  order, each picture in the bitstream buffer handed in with the k-th
  picture; the decode time of the k-th picture out is two pictures before
  the time of the k-th picture given (the rule ffmpeg uses: at a steady
  frame rate the time of the picture given two earlier, but it does not
  reach back across a gap in the input), never after its own time. No
  look-ahead. Rate control is VBR capped at the bit rate, with the same
  buffer as on the CPU.
- If the card will not decode a stream (a format it does not take, no
  decoder to be had) and nobody insisted on the card, the CPU decoder
  takes over from the very unit that failed; the encoder stays on the
  card and gets its pictures by upload. The encoder holds the CUDA
  context in its own right, so that the decoder going first does not
  take the context from under it.
- If the encoder will not open and nobody insisted on the card, the
  stream's decoder is replaced by the CPU one and encoding goes to the
  CPU too. One group of pictures is lost at the switch.
- Interlaced H.264 is encoded as fields if the card says it can. If
  initialising then fails, a fresh session encodes the fields together as
  frames rather than losing the card for the stream.

`tests/nvmock.c` builds stand-ins for the three libraries that enforce
the API's rules (structure versions, call order, context, mapping limits)
and keep "card" memory in ordinary memory of exactly the size asked for,
so that AddressSanitizer sees any overrun. The self-test and whole channel
runs pass against them. That proves our side is consistent with our
reading of the headers. It does not prove the reading is right. In
particular the stand-ins cannot show: the layout of a decoded surface
(they reproduce our own assumption), time stamps on pictures whose access
unit had none, the reuse of a picture index while that picture still
waits to be fetched (possible when one PES packet holds several
pictures), field mode, rate control, and which levels and presets a real
card accepts. Those are for the first run on a card.

**Target** (facts for the first deployment box, driver 535.247.01, two
Tesla P40): Video Codec SDK 12.1 headers, which need driver 530.41.03 or
newer; 12.2 needs 550.54.14. A P40 has two encoder engines and
unrestricted sessions, encodes H.264 and H.265 including 10-bit, and has
no H.265 B-frames and no AV1.

### Decisions still open

**HE-AAC** (decided 2026-10-14). Ubuntu ships fdk-aac with the HE-AAC
parts removed, and a look at the live multiplex showed nearly every
service's audio to be HE-AAC at 48 kHz. The installer now builds the
original fdk-aac from source (full band in; HE-AAC output possible later).
The alternative, decoding AAC through libavcodec, was not taken: it meant
a new backend to write and verify, and no HE-AAC output.

**B-frames on NVENC** (done 2026-10-14, after the first hardware run):
two for H.264, none for H.265 on this card generation.

### Licensing, to check before anything is distributed

x264 and x265 are GPL. fdk-aac's licence is not GPL-compatible. Ubuntu's
libavcodec is built with GPL parts enabled. Running all of them on your
own servers is not distribution and raises no issue. Shipping a binary
that links them to a third party would. This is not legal advice; it is a
flag to raise with whoever advises you before a binary leaves your hands.

The NVIDIA headers in `third_party/ffnvcodec` are under the MIT licence
(see `third_party/ffnvcodec/README`).
