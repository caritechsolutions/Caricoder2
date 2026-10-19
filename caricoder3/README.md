# caricoder3

A live MPEG-TS to MPEG-TS processor and transcoder written from the ground
up in C. No GStreamer and no FFmpeg pipeline: the demultiplexer, the
multiplexer, the clock and all handling of time are our own. Codec
libraries are called directly, each behind a small interface, and each is
optional: the program builds and runs with whichever are installed.

This is the foundation for replacing Flussonic's transcoder. It lives in its
own folder and shares no code with the GStreamer-based CariTranscoder in the
rest of this repository.

## Where it stands

| Stage | What | State |
|---|---|---|
| 1 | TS in → demux → remux → TS out, every stream copied | done; ran 34 hours on a live satellite feed |
| 2 | Audio transcode: MP2, MP3, AAC, AC-3 in; AAC, MP2, MP3 out | done, tested |
| 3a | Video transcode on the CPU: H.264, H.265 in and out | done, tested |
| 3b | Video transcode on NVIDIA cards (NVDEC, NVENC) | done; first run on a Tesla P40 2026-10-14, B-frames since then only against stand-ins (see "NVIDIA") |
| 3c | Video on Intel (VPL) | not started |
| 4 | Several renditions of a channel in one stream (scaling, deinterlace, aligned key frames) | done on the CPU and against the stand-in card; not yet run on a real card |
| 5a | Outputs added and removed while the channel runs; several outputs per channel; control socket; config file | done, tested (UDP, RTP, file) |
| 5b | SRT and RIST outputs (caller and listener, encryption, reconnect) | done, tested against srt-live-transmit and ristreceiver |
| 5c | RTMP push (H.264 + AAC) | done, tested against ffmpeg as the server; not yet against a public RTMP service |
| 6 | HLS packager as an output (TS segments, master playlist for renditions, served by the process, optional directory) | done, tested; played by ffmpeg's HLS client |
| 7 | Supervisor (`caricoder3 serve`): runs the channels, REST API, GUI on port 8550, HLS for every channel on that port | done, tested; side-by-side run against Flussonic not done |
| 9 | Shared inputs (one reader per MPTS, `caricoder3 share`); HTTP TS input; output PIDs, PMT order, PMT/PCR PID, service/TS/network ids; preview player (300k H.264, audio choice), stream info and bit-rate graph | done, tested in the lab (2026-10-18); shared inputs and preview running on transcoder2 |
| 8 | SRT and RIST inputs; MPEG-2 video decode; `caricoder3 probe` and "Scan source" in the GUI with stream selection (`--drop`); server page (CPU, memory, GPU, interfaces); list filter | done, tested in the lab (2026-10-16); not yet run on transcoder2 |

Picture quality has been measured on real streams with `tests/quality.py`
(see "Picture quality"): 45 dB PSNR for HEVC → H.264 on the CPU at 2.5× the
source rate, 43 dB for HEVC 960x720 → HEVC 1080p at the source rate on NVENC.

## Install on a server

```
curl -fsSL "https://raw.githubusercontent.com/caritechsolutions/Caricoder2/claude/caricoder3-stage2/caricoder3/install.sh?$(date +%s)" | sudo bash
```

Needs no git. It installs gcc and make if they are missing, and the codec
libraries (below); builds in a scratch folder; and only if that succeeds
puts the source in `/opt/caricoder3` and the binary in
`/usr/local/bin/caricoder3`. It starts and stops nothing; running it again
upgrades. It ends by listing what the build can do. Add `-s -- --test`
after `bash` to run the self-tests before anything is installed, or
`-s -- --no-codecs` for the plain remultiplexer. Keep the `?$(date +%s)`:
without it GitHub may hand you a cached, older script.

## Build by hand

```
cd caricoder3
make            # -> ./caricoder3
make test       # builds the sanitizer version too and runs the test suite
sudo make install
./caricoder3 --backends     # what this build can decode and encode
```

Only gcc and make are required. Each codec library is used if it is
installed and left out if it is not:

| For | Library | Ubuntu package |
|---|---|---|
| AAC decode and encode | fdk-aac 2.0.3 | built from source by the installer (see "HE-AAC") |
| MP2 / MP3 decode | mpg123 | `libmpg123-dev` |
| AC-3 decode | liba52 | `liba52-0.7.4-dev` |
| MP2 encode | twolame | `libtwolame-dev` |
| MP3 encode | LAME | `libmp3lame-dev` |
| H.264 / H.265 decode on the CPU | libavcodec | `libavcodec-dev libavutil-dev` |
| H.264 encode on the CPU | x264 | `libx264-dev` |
| H.265 encode on the CPU | x265 | `libx265-dev` |
| H.264 / H.265 on NVIDIA | the driver's own libraries | nothing to install |

plus `pkg-config` to find them. `make CODECS=none` builds without any,
`make NVIDIA=0` without the NVIDIA part.

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

# Audio to AAC at 128 kb/s, video copied
caricoder3 -i udp://239.5.5.5:5000 -o udp://239.6.6.6:6000 --audio aac:128k

# Only the AC-3 stream on PID 0x103 to AAC; everything else copied
caricoder3 -i ... -o ... --audio 0x103=aac:96k

# Video to H.264 at 3 Mb/s and audio to MP2, constant 4 Mb/s out
caricoder3 -i ... -o ... --video h264:3M --audio mp2:192k --muxrate 4M

# The same, video on NVIDIA card 1
caricoder3 -i ... -o ... --video h264:3M --audio mp2:192k --muxrate 4M --gpu 1

# Two outputs, and a control socket to add or remove outputs later
caricoder3 -i ... -o udp://239.6.6.6:6000 -o udp://10.0.0.9:6000 --name vacation
caricoder3 ctl vacation add-output udp://239.6.6.7:6000
caricoder3 ctl vacation list-outputs
caricoder3 ctl vacation remove-output out3
```

`caricoder3 --help` lists every option. The ones that matter most:

| Option | Meaning |
|---|---|
| `--muxrate RATE` | CBR output at this rate (`5M`, `4500k`). Must exceed the content rate by a few percent |
| `--vbr` | With `--muxrate`: VBR capped at that rate |
| `--delay MS` | Input-to-output buffer. Default 500; 800 when audio is transcoded, 3000 when video is. See "Delay" below |
| `--program N` | Service to take. Default: the first one in the PAT |
| `--audio SPEC` | Transcode audio to `aac`, `mp2` or `mp3`, optionally with a bit rate (`aac:128k`). `PID=SPEC` for one stream; may be repeated. Default: copy |
| `--video SPEC` | Transcode video to `h264` or `hevc` with a bit rate (`h264:3M`). Default: copy |
| `--decoder`, `--encoder` | Where video is decoded and encoded: `auto` (the card if there is one, else the CPU), `nvidia`, `cpu` |
| `--gpu N` | Which NVIDIA card. Default 0 |
| `--preset NAME` | Encoder speed, in the encoder's own words: x264/x265 `ultrafast` … `medium` (default `veryfast`), NVENC `p1` … `p7` (default `p4`) |
| `--gop MS` | Longest distance between key frames. Default 2000 |
| `--vbv MS` | Video decoder buffer, as time at the bit rate. Default 1000 |
| `--control PATH`, `--name NAME` | A Unix socket for adding and removing outputs while the channel runs; `--name` puts it at `/run/caricoder3/NAME.sock`. See "Outputs" |
| `--config FILE` | JSON file of outputs, read at start and on `SIGHUP`; outputs added with `--save` are written to it |
| `--stats-file PATH` | JSON status, rewritten atomically once a second |
| `--thumb PATH` | A 320-wide JPEG of the picture, rewritten every `--thumb-interval` s (default 5): from the transcoder's decoder, or one key frame decoded on the CPU when the video is copied. The supervisor sets it; the GUI shows it on the list and the channel page |
| `--rt-prio N` | Real-time priority for the output thread (needs `CAP_SYS_NICE`) |

| `--drop PID[,PID..]` | Leave these streams of the service out of the output: an audio track not wanted, say. `caricoder3 probe` lists what there is |

Input URLs accept `udp://` and `rtp://`, a source for source-specific
multicast (`udp://10.0.0.1@232.1.1.1:5000`), and `?iface=`, `?buf=`.
RTP is detected per datagram, so a `udp://` URL works for an RTP source.
The receive socket binds to the group address itself, so two streams that
share a port number do not get mixed.

`-i` may be given more than once: the first is the primary, the others
stand by and are tried in turn when it delivers nothing for
`--switch-after` ms (default 3000); the primary is tried again every
`--retry-primary` s (default 60) and taken back as soon as it delivers.
A switch is a source restart to the engine: the output time line stays
continuous. The statistics list the sources and which is in use; the
GUI's lamp goes yellow while a backup carries the channel. Channel file:
`"backup_inputs": [...]`, `"switch_after"`, `"retry_primary"`.

SRT and RIST sources use the same URL forms as the outputs (see
"Outputs"): `srt://host:port` calls a sender, `srt://:port` listens for
one (`?latency=`, `?passphrase=`, `?pbkeylen=`, `?streamid=`);
`rist://@:port` listens, `rist://host:port` calls (`?secret=`,
`?aes-type=`, `?buffer=`, `?profile=`). An SRT listener takes one sender
at a time: another is refused while the first delivers, and replaces it
once the first has been silent for a second (an encoder that restarts
and pushes again gets in without waiting for a timeout). The link's
state, RTT, recovered and lost packets and reconnects are in the
statistics (`input.link`) and on the channel's Source card.

An HTTP source, `http://host[:port]/path` (`http://user:pass@host/...`
for basic authentication), pulls a TS stream from a server such as
Flussonic's `http://SERVER/NAME/mpegts`: plain or chunked, redirects
followed. The burst of buffered stream a server starts with is skipped
(until the PCR moves at the speed of the clock, at most 10 s), so the
channel starts at the live edge; when the connection ends, breaks or is
silent for 5 s it connects again after a second. Not HTTPS.

### What is in a source

```
caricoder3 probe udp://224.100.1.5:49500            # listens 3 s, prints JSON
caricoder3 probe srt://:9000 --seconds 5
caricoder3 probe recording.ts --program 1840
```

Every program of the multiplex with its name, PCR PID and streams; for
each stream the PID, type, kind (video, audio, subtitles, teletext,
data), language, bit rate, and what the bytes say: picture size,
interlace, frame rate and profile for video, sample rate and channels for
audio (libavcodec's parsers read the headers, nothing is decoded). The
GUI's "Scan source…" button on a channel's settings runs the same and
lists the programs to pick one, with a tick box per stream: unticked
streams become the channel's `drop` list. Subtitles, teletext and data
streams stay ticked unless you say otherwise.

Output URLs accept `?ttl=` (default 8), `?iface=`, `?buf=`, and `?pkt=`
(TS packets per datagram, default 7).

### Outputs

`-o` may be given more than once. Every output runs in a thread of its
own, reading the muxed stream from a shared ring of packets, so a slow
or dead output (a unicast port nobody listens on, a full socket buffer)
loses its own packets and nothing else: the others, and the pipeline,
do not notice. Packets missed that way are counted as `overruns` for
that output.

Outputs can be added and removed while the channel runs, over a Unix
socket (`--control PATH`, or `--name NAME` for `/run/caricoder3/NAME.sock`),
with the same binary as the client:

```
caricoder3 ctl NAME list-outputs
caricoder3 ctl NAME add-output udp://239.6.6.7:6000 [ID] [--save]
caricoder3 ctl NAME remove-output ID
caricoder3 ctl NAME stats
caricoder3 ctl NAME reload
```

The socket speaks one JSON object per line and answers with one:
`{"cmd": "add-output", "url": "udp://...", "id": "cdn", "save": true}`
→ `{"ok": true, "id": "cdn"}`; `{"cmd": "list-outputs"}` → the same
array as `outputs` in the statistics. An output added with `--save` is
written to the `--config` file (`{"outputs": [{"id", "url"}, ...]}`),
which is read at start and again on `SIGHUP` or `reload`, so outputs
survive a restart and can be edited by hand; outputs given with `-o` or
added without `--save` are not in the file. A new output starts with
the next packet the mux produces (it is not sent what was already
sent), and the moment it is removed its last datagram goes out and its
thread ends; the other outputs see nothing of either.

Under the supervisor all of this is done for you by the GUI; the
commands below are what it runs. Output types (`caricoder3 --backends`
lists what the build has):

| URL | What |
|---|---|
| `udp://host:port[?ttl=&iface=&pkt=&buf=&loop=0]`, `rtp://...` | plain UDP or RTP, unicast or multicast; `loop=0` keeps a multicast output off this machine (Flussonic's `multicast_loop=false`): a receiver here does not get it |
| a file path | written as fast as it comes |
| `srt://host:port[?latency=120&passphrase=...&pbkeylen=16&streamid=...]` | SRT caller: connects, and connects again by itself when the link breaks (`reconnects` in the stats; datagrams dropped meanwhile count as `errors`) |
| `srt://:port` or `srt://host:port?mode=listener` | SRT listener: up to 8 receivers may call in; each gets every datagram, a slow one loses its own |
| `rist://host:port[?profile=main&buffer=1000&secret=...&aes-type=128&bandwidth=...]` | RIST caller (librist; main profile by default, `simple` or `advanced` on request). The parameters after `profile` are librist's own |
| `rist://@:port[?...]` | RIST listener (librist's notation) |
| `hls://[:port][/name][?segment=4&window=6&dir=PATH&keep=20]` | HLS: TS segments cut at key frames (every `segment` seconds, the last `window` kept), one variant per program of the output (a master playlist for a channel with renditions), served by this process on `port` and through the supervisor; `dir=` also writes segments and playlists there for nginx or a CDN origin. See "HLS" |
| `rtmp://host[:1935]/app/stream[?program=N]` | RTMP push of the program's H.264 video and AAC audio (what RTMP carries; HEVC and other audio are left out, and the log says so). Reconnects by itself; a server that cannot keep up costs it its own frames (`errors`) until the next key frame |

SRT and RIST outputs report what their libraries measure: `rtt_ms`,
`retransmitted`, and for SRT `lost`. For an RTMP output `packets` counts
FLV tags.

### HLS

An `hls://` output is a packager and a small web server in one. It cuts
the output into MPEG-TS segments at video key frames (so `--gop` sets the
shortest possible segment; with several renditions their key frames are
aligned and so are their segments), keeps the newest `window` segments
in memory and serves:

```
http://host:PORT/index.m3u8        the media playlist (one program) or the
                                   master playlist (several: one variant per
                                   program, with BANDWIDTH, RESOLUTION, FRAME-RATE)
http://host:PORT/v0/index.m3u8     variant 0's media playlist
http://host:PORT/v0/seg123.ts      a segment
```

Each variant is a clean single-program stream: its own PAT and SDT, the
program's PMT and PIDs, no null packets; every segment opens with the
tables and a key frame, so any segment plays on its own. An audio-only
program is cut on time. Playlist answers carry `X-Next-Segment`, the name
of the segment that will be listed next, so a proxy in front can fetch
it the moment it appears.

With `--name`, the same is served on a Unix socket next to the control
socket, which the supervisor (stage 7) serves under `/hls/NAME/` on its
own port, so one port serves every channel. With `dir=`, segments and
playlists are also written to that directory (atomically), the newest
`keep` segments kept, for a web server or CDN origin of your own.

### Under systemd

```ini
[Unit]
Description=caricoder3 channel %i
After=network-online.target

[Service]
Type=notify
ExecStart=/usr/local/bin/caricoder3 -i udp://239.5.5.5:5000 \
          -o udp://239.6.6.6:6000?ttl=8 --muxrate 5M \
          --stats-file /run/caricoder3/%i.json \
          --name %i --config /etc/caricoder3/%i.outputs.json
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

## The supervisor and the GUI

`caricoder3 serve` is one process for the machine: it runs every channel
described in `/etc/caricoder3/channels/*.json` as a child process,
restarts one that dies (after 1, 2, 4 … 30 s), and serves on TCP port
**8550** (set in `/etc/caricoder3/caricoder3.json`):

- `/` — the GUI: plain HTML and JavaScript, built into the binary. Channel
  list with state and rates, filtered by name or health; per channel the
  live statistics, the outputs (add and remove while it runs), the
  settings form with "Scan source…", the log; a Server page with CPU,
  memory, the NVIDIA cards (through `nvidia-smi`) and the network
  interfaces, sampled every two seconds.
- The **player** sits in the Source box of a channel, with the channel's
  snapshot as its poster (hls.js, built in, Apache-2.0). Play starts the
  *preview*: a separate small process makes the channel into 426x240
  H.264 at 300 kb/s with one AAC track, in 1 s HLS segments, so it starts
  in a few seconds and plays in any browser. When the channel has several
  audio tracks a list picks the one heard (the preview restarts with it).
  The preview reads a multicast input itself; for any other input it
  reads a loopback copy of the channel's output (a hidden `pvtap`
  output). It stops when the player stops, when the page is left, or
  after a minute without requests. **Watch original** plays the channel's
  own stream instead: its HLS output when it has one, otherwise a
  temporary `hls://?segment=10` output (about 30 s behind the air; HEVC
  plays only where the browser decodes it).
- The **Streams** card lists the input and output streams (kind, codec,
  size, language, rate, PID) beside a graph of the input and output bit
  rates over the last two minutes.
- **Shared inputs.** Channels whose input is the same UDP/RTP URL (an
  MPTS that 30 channels take programs from, say) do not each read it:
  the supervisor runs one reader per input (`caricoder3 share`), which
  joins it once and sends every channel only its program (PAT, SDT, its
  PMT and that PMT's PIDs) over a loopback port. Channels added and
  removed change the reader without a gap for the others; a reader that
  dies is started again in a second; the last channel gone stops it. A
  unicast input can feed several channels this way too. Backup sources
  are read by the channel itself, as before. `"share_input": false` on a
  channel makes it read its input itself (for a unicast input that others
  share, that takes the input from them). The Server page lists the
  shared inputs, their rate and channels.
- `/api/...` — the same, as JSON (see below)
- `/hls/NAME/index.m3u8` — every channel's HLS output, through one port

The installer sets it up as the `caricoder3` systemd service, enabled
and started. The GUI asks for a password: user `admin`, password made
up on the first run and kept in the config file:

```
caricoder3 serve --show-password
caricoder3 serve --password MyNewPassword      # change it (then restart the service)
```

A channel file holds what the command line would, for example
`/etc/caricoder3/channels/vacation.json`:

```json
{
  "input": "udp://224.100.1.5:49500?iface=eth1",
  "program": 1840,
  "muxrate": "5M",
  "video": ["h264:3M", "640x360:h264:1M"],
  "audio": ["aac:128k"],
  "gpu": 0,
  "service_name": "Vacation",
  "enabled": true
}
```

Keys: `input`, `backup_inputs` (list), `switch_after`, `retry_primary`, `program`, `drop` (list of PIDs), `muxrate`, `vbr`, `delay`, `video` (list),
`audio` (list), `decoder`, `encoder`, `gpu`, `preset`, `bframes` (0–4),
`profile` (`baseline`, `main`, `high`; `main10`), `gop`, `vbv`,
`pcr_interval`, `psi_interval`, `service_name`, `service_provider`,
`service_id`, `ts_id`, `onid`, `pmt_pid`, `pcr_pid` (a PID, or `"video"`),
`remap` (`{"256": 481, ...}`: input PID to output PID), `order` (input
PIDs in the order the PMT lists them; receivers play the first audio),
`share_input` (default true), `extra_args` (list, passed as they are), `enabled`. The output's
identifiers are set in the GUI under "Timing and service details"; a PID
not asked for that meets one that is moves to the next free PID (logged). The channel's outputs
live next to it in `vacation.outputs.json`, which the channel itself
reads and writes (the `--config` file), so an output added in the GUI
survives a restart. The supervisor re-reads the directory on `SIGHUP`
(`systemctl reload caricoder3`) and when the GUI or API changes a
channel; a running channel whose file changed is restarted.

The GUI and the API are plain HTTP with basic authentication: fine on a
management network, not something to put on the public internet as it
is (a reverse proxy with TLS in front, or a firewall rule, is the way).
The `/hls/` paths are open, as players cannot sign in. Anyone with the
password can make the machine write files (`file:///path` outputs,
`hls://?dir=`), so the password is a root password for this purpose.

The API (basic auth, JSON in and out):

| | |
|---|---|
| `GET /api/system` | version, host, uptime, channels, what this build can do, and `system`: CPU, memory, GPUs, interfaces |
| `POST /api/probe` | `{"url": "...", "seconds": 3, "program": 0}`: what is in a source (runs `caricoder3 probe`; up to 30 s) |
| `GET /api/channels` | every channel: state, pid, uptime, restarts, config, statistics, outputs |
| `GET /api/channels/NAME` | the same for one, plus its last 200 log lines, `history` (input/output bit rate every 2 s, last 2 min) and `preview` (running, uptime, drop) |
| `PUT /api/channels/NAME` | create or replace the channel (body: the JSON above) |
| `DELETE /api/channels/NAME` | stop it and remove its files |
| `POST /api/channels/NAME/start`, `/stop`, `/restart` | |
| `POST /api/channels/NAME/outputs` | `{"url": "...", "id": "...", "save": true}`: add an output (`"save": false` for one that should not survive a restart) |
| `PUT /api/channels/NAME/outputs/ID` | `{"url": "..."}`: change one in place (out and in at once; a URL that will not open leaves the old one); saved or not as before. The GUI's Edit |
| `DELETE /api/channels/NAME/outputs/ID` | remove one |
| `GET /api/channels/NAME/log` | the last lines it wrote |
| `POST /api/channels/NAME/preview` | `{"drop": [pid, ...]}`: start the preview (or restart it leaving out other streams); answers `{"url": "/hls/NAME.preview/index.m3u8"}` |
| `DELETE /api/channels/NAME/preview` | stop it |
| `GET /api/channels/NAME/thumb.jpg` | a picture of the channel, up to 5 s old (320 wide) |
| `GET /hls/NAME/...` | the channel's HLS output (no password) |
| `GET /api/channels/NAME/history?from=&to=` | the record on disk (default the last 24 h): `rows` of `[t, in_kbps, in_max_kbps, out_kbps, out_max_kbps, cc, overflow, dropped, late, lost, switches, restarts, health, up_s]`, about 720 of them, 10 s, 1 min or 1 h apiece by span |
| `GET /api/history/server?metric=cpu*&from=&to=` | the server's: `cpu.percent`, `load.1`, `mem.used_pct`, `gpuN.util/encoder/decoder/mem_mb/temp_c/power_w`, `net.IF.rx_mbps/tx_mbps/rx_drop/rx_err`; a name or a prefix ending in `*` |
| `GET /api/events?channel=&from=&to=&limit=` | what happened, newest first (no `channel`: everything) |
| `GET /api/incidents?channel=&open=1&from=&to=&limit=` | problems that lasted (default the last 7 days), open ones first; a channel's own and its source's |
| `GET /api/incidents/ID` | one, with the channels' incidents that belong to it and the events from a minute before to a minute after |
| `GET /api/watchdog`, `PUT /api/watchdog` | the watchdog's limits and rules (PUT: any of them, kept in the config), and what it is watching now |

### History and events

The supervisor keeps a record in one SQLite file (`"history"` in its
config, default `/var/lib/caricoder3/history.db`; `"none"` turns it
off): every 10 s each channel's rates and error counts and the server's
CPU, memory, GPU and network figures, summed up per minute and per hour;
and an event log: starts, stops, failures, input lost and back, switches
to a backup source, warnings and errors the channels wrote (the same
line again within a minute is counted, not repeated), and changes made
in the GUI. Detail is kept 3 days, minutes 30 days, hours and events a
year. The file never grows past `"history_max_mb"` (default 2048; 120
channels take about 42 MB a day): when it would, the oldest detail goes
first and events last. The GUI shows it in a History card on each
channel and on the Server page (1 h to 30 days). The on-server AI
technician, when it comes, will read it too.

### The watchdog and incidents

Every 2 s the supervisor checks each channel against a set of rules: no
input (after 5 s), CC errors (10 or more in a minute), packets lost on
the way in, dropped or late pictures (more than 5 a minute), output
stalled while input arrives, the channel failing or restarting again
and again (3 times in 10 minutes), running on a backup source, the
clock not locked, a black, frozen or silent picture or sound, a stream
the PMT lists without data, the source's PIDs changing; and the server:
CPU or a video engine above 90% for 2 minutes, a network card dropping
packets. A problem that lasts 10 s is an **incident**, kept in the
history with the channel's statistics as they were when it began; it is
over when the problem has been gone 30 s. Shorter ones are only events.
When channels that read the same source have the same upstream problem
within a minute of each other, one incident for the source is opened as
well and theirs belong to it ("no input on 3 channels of
udp://239.1.1.1:5000: a, b, c"): the sign that the cause is before the
transcoder. The picture, sound and missing-stream rules only count while
input arrives. The limits and which rules are in use are set on the
Server page (Watchdog, "Change") or in the config's `"watchdog"` object
(`min_s`, `clear_s`, `group_s`, `no_input_s`, `cc_per_min`,
`dropped_per_min`, `restarts`, `restart_window_s`, `cpu_pct`, `gpu_pct`,
`load_s`, `off`: rule ids). The Channels page lists the last 24 hours'
incidents, each channel's History card its own; a click shows one in
full. Nothing is sent anywhere yet. Built without
`libsqlite3-dev`, none is kept.

## Transcoding

A stream is either copied or transcoded; the two mix freely in one
service. What is transcoded keeps its PID and its place in time: time
stamps pass through, so audio and video stay where they were against each
other. The PMT is rewritten for a transcoded stream (stream type, and only
its language and stream-identifier descriptors kept).

**Audio.** In: MPEG audio layers I to III, AAC (ADTS and LATM), AC-3.
Out: AAC-LC, MP2, MP3. The sample rate is kept; mono stays mono; anything
with more than two channels is mixed down to stereo. A bit rate the coding
does not allow at that sample rate is moved to the nearest it does. Lost
input is filled with silence so that the output keeps its rhythm.
Measured with ffmpeg as the reference decoder, audio lands within 0.02 ms
of where the original sat, for every pairing.

**Several renditions of one channel** (for IPTV/OTT): `--video` may be
repeated with a size in front, and each becomes a program of its own in
one output transport stream, with the audio and the PCR shared:

```
caricoder3 -i udp://... -o udp://... --audio aac:128k \
    --video h264:4M --video 1280x720:h264:2M --video 854x480:h264:900k
```

Program 1 is the full-size version with the source's numbers and PIDs;
the others take the next program numbers that no service of the input
has, and PIDs above everything the input uses, so a stream the source
adds later cannot land on them; the SDT names them "Channel 1280x720"
and so forth. Key frames fall on the same pictures in every rendition, every
`--gop` (2 s), so a packager cuts aligned segments. Each rendition is a
transcoder of its own with its own decoder (the card's decoder scales as
it decodes; on the CPU an area-averaging scaler follows libavcodec), so
three renditions decode the source three times. Interlaced sources come
out progressive in a scaled rendition, at one frame per source frame.

A size may leave one side to the source's display shape: `-1x720` is 720
high and as wide as the shape asks (an anamorphic 960x720 shown 16:9
becomes 1280x720, a 4:3 one 960x720), `1280x-1` the other way round, and
`1280x720:fit` the largest picture with the source's shape that fits in
the box. The shape comes from the source's own signalling (the aspect
ratio in its video), and the output carries its shape the same way: a
scaled picture's pixels are made square when the shape allows, within
half a percent. The GUI's resolution list offers these; the SDT names
such a rendition "Channel 720p".

**Video.** In: H.264, H.265. Out: H.264, H.265. Picture size, frame rate,
pixel shape, field order and colour description are kept, and followed
when the source changes them while running (a channel going from 4:3 to
16:9, say: the encoder is restarted at that picture). H.264 goes out at
8 bits (10-bit input is converted); H.265 keeps the depth of the source.
Every picture comes out at the time stamp it came in with.

New video is sent at its own bit rate: a large key frame is spread over
the time it takes at that rate instead of being pushed out at once, and a
receiver never has to hold more than the encoder's buffer (`--vbv`, 1 s by
default). The pace is the bit rate plus 1.5 %. It rises further only when
the schedule is well behind: for a short while after the largest pictures
(1.4 times the bit rate over 0.4 s was the most measured, transport
packets and all), and when the encoder itself runs over its rate (x265
does, by a few per cent). New audio is packed and timed
to stay inside the 3584 bytes a standard decoder has for audio, and is
kept at least 75 ms ahead of its time; only MP2 at 320 and 384 kb/s needs
more than the buffer for that, and there being ahead wins.

**Delay.** Decoding and encoding take time, and an encoder holds pictures
back to look ahead. `--delay` defaults to 800 ms when audio is transcoded
and 3000 ms when video is (more with a `--vbv` above 1000: the two grow
together, and a `--delay` shorter than `--vbv` plus half a second is
refused). The statistics show, per transcoded stream, the
least time a unit had to spare when it reached the multiplexer
(`slack_min_ms`). If that is large, `--delay` can come down by about that
much. If it is negative, units reached the multiplexer after their send
time: they go out late, and are dropped if later than the multiplexer
allows. When that lasts, the log says which of two things is wrong:

- "`--delay is too short`": the codec keeps up, but holds back more
  pictures than the delay allows for. Raise `--delay`.
- "`slower than the stream`": the codec cannot keep up at all (too slow a
  `--preset` for the machine). Input that could never be caught up on is
  thrown away and counted (`input_dropped`), so that what does come out is
  current; the other streams of the service are not affected. A codec
  that is behind but gaining (after its start, or after a stall) is left
  to catch up.

The CPU codecs are held to a few threads per stream (4 to decode, 6 for
x264), because every extra thread is one more picture held back. The
delay is therefore the same on a 72-processor server as on a small
machine; measured at 1080p, the video has about 1.5 s of the 3 s to spare.

Video in: H.264, H.265 and MPEG-2 (stream types 0x01/0x02, progressive
or interlaced), on the CPU or the card. **Not decodable here** (the
stream is copied, with a warning): E-AC-3, anything else.

### HE-AAC

The feeds this is built for carry HE-AAC, and Ubuntu's `libfdk-aac`
package is a build with the HE-AAC parts removed ("fdk-aac-free"): with it
an HE-AAC input is decoded from its half-rate core only, nothing above
12 kHz comes through, and HE-AAC v2 comes out mono. So the installer
builds the original fdk-aac (2.0.3) from source into
`/opt/caricoder3-libs`, statically, and the Makefile prefers it. With it
HE-AAC v1 and v2 come out full band and in sync (tested to 0.02 ms).
`caricoder3 --backends` says which it got: "incl. HE-AAC" or "NO HE-AAC",
and the log warns on a 24 kHz AAC stream when the library cannot. The
licence is Fraunhofer's own, not GPL; running it on your own servers is
not distribution (see the end of `docs/DESIGN.md`).

### NVIDIA

**The NVIDIA code first ran on a card (Tesla P40, driver 535) on
2026-10-14**: the self-test passed all five rounds, and a live channel
(HEVC in, H.264 out on the card, HE-AAC to AAC) ran clean; the B-frame
part (interlaced with B-frames in particular) came after that run. It was written
on a machine without a card. What was done before that: it compiles against the
headers of NVIDIA's Video Codec SDK 12.1 (needs driver 530.41.03 or
newer), and it runs against stand-in libraries (`tests/nvmock.c`) that
hold it to the rules of the API as the headers state them: structure
versions, the order of calls, a context current around each, no more
pictures mapped than asked for, no copy outside its memory. That shows
our side is consistent. Only a card can show that the driver agrees.

The first thing to run on a machine with a card:

```
caricoder3 --gpu-test            # card 0
caricoder3 --gpu-test --gpu 1 -v
```

It needs no input. It makes pictures itself and takes them through the
same code a channel uses: encode from memory, decode on the card, encode
again without leaving the card, for H.264, H.265, 10-bit H.265 and
interlaced H.264, and says after each step whether it worked and what the
driver answered.

How it is built: pictures decoded on the card stay on it and are copied,
card to card, into the encoder's input. H.264 is coded with two B-frames
between references (about a tenth off the bit rate for the same quality;
the card holds back two pictures for it); H.265 without, as the Tesla P40
has no H.265 B-frames. No look-ahead. When the card lets a stream down
and nobody insisted on the card (`--decoder nvidia`, `--encoder nvidia`),
the CPU takes over and the log says so: if the card will not decode the
stream (a format it does not take, no decoder free), the CPU decodes and
the card still encodes; if the card's encoder will not open (no session
free, say), decoding and encoding both carry on on the CPU. With the card
insisted on, the stream is silent instead, which is said once and shown
as `failed` in the statistics. If the card refuses to encode interlaced
video as fields, it encodes the fields together as frames, with a
warning. `--decoder cpu --encoder nvidia` works (pictures are uploaded);
`--decoder nvidia --encoder cpu` is not built yet.

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
- If the source's clock steps (a restart, a stall that stays late, a skip
  forward), the output clock runs on unbroken and the content is spliced
  onto it. PTS/DTS are shifted by one constant; nothing else in the PES
  packets changes. Content that arrives while the step is being confirmed
  (a third of a second with ordinary PCR spacing) is held, then sent on
  the new clock: with PCRs up to about a quarter of a second apart,
  nothing is dropped but the PES packet on each stream that the step
  itself cut in two. The cases this does not cover are under "Known
  limits".
- A single PCR with a wrong value (a damaged packet) does not break the
  stream or move the time line, whether it is seconds or a few
  milliseconds out. A PCR that does not follow on from the last one is
  only acted on once the next PCR agrees with it.
- Input that is merely held up and then arrives in a burst, or is caught
  up at 1.2 times real time or faster, is not mistaken for a new clock:
  the time line is left alone.
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
- If the source moves its streams to other PIDs (the provider rebuilt the
  multiplex), each stream is recognised by what it is (video, audio,
  language, its place among streams of that kind) and keeps the PID it
  had: the channel's settings, which name the old PIDs, still apply, and
  the output's PIDs do not change. The PMT and a PCR-only PID are kept
  the same way. It is logged ("source PIDs changed: audio eng moved from
  PID 0x0101 to 0x0102; kept as 0x0101"), and the GUI shows the source's
  PID next to the old one. Under the supervisor what was learnt is kept
  in `channels/NAME.streams`, so a channel started after the change still
  knows; delete that file (with the channel stopped) to have the channel
  take the source as it is now. A stream on a PID the settings name is
  always taken as that stream.
- A video or audio stream the PMT lists but nothing of which arrives
  while the rest does is reported once after 5 s, and again when it is
  back; the GUI marks it "no data".
- The content is watched: a picture that is black, or does not change
  at all, for 10 s, and an audio stream that is silent (peaks under
  -60 dBFS) for 10 s, are reported once and again when they end, and
  shown on the channel. The picture is looked at when a thumbnail is
  made (every 2 s under the supervisor; on a channel that copies its
  video, one key frame is decoded on the CPU for it). Each audio stream
  is decoded on the CPU one second in every three. A still that is meant
  to be still (a slate, a menu) counts as frozen too.

### Delay

Everything waits `--delay` milliseconds between input and output. That is
what allows a whole PES packet to be collected before it is sent and still
leave on schedule. The default of 500 ms reproduces the input's own pacing
exactly. A smaller value still gives a valid, complete stream (tested at
60 ms); large frames then leave in a burst instead of evenly. Transcoding
needs more: see "Transcoding" above.

## Statistics

`--stats-file` produces one JSON object:

- `state`: `waiting_for_input`, `waiting_for_clock` or `running`
- `input`: bit rate, age of the last packet, continuity errors on this
  service and on the rest of the multiplex, sync losses, kernel socket
  overflows, damaged PES dropped, bad tables, PCR jumps, single damaged
  PCRs set aside (`pcr_glitches`), PCR missing
- `service`: PIDs, stream types, how each stream is handled; for a
  transcoded stream also the decoder and encoder in use, units and frames
  in and out, frames that would not decode, input dropped because the
  codec fell behind, silence inserted, and the time units had to spare
  (`slack_ms` since the last report, `slack_min_ms` since the start)
- `clock`: locked or not, measured drift against the source in ppm, arrival
  jitter, time-line offset, number of discontinuities absorbed, units held
  back at this moment because the source clock is in doubt
- `outputs`: one entry per output: id, type, URL, state, packets and
  bytes handed to the transport, send errors, overruns (packets missed
  for being too slow), whether it is saved in the config file
- `output`: mode, bit rate, share of null packets, units dropped for lack
  of output rate, units that left after their own DTS, clock skips after a
  process freeze, queue depth, send errors (all outputs together)

The same figures go to the log every 10 seconds (`--stats-interval`).

## Picture quality

`tests/quality.py` compares a transcoded stream with its source frame by
frame (matched by PTS, which caricoder3 passes through unchanged) and
reports PSNR and SSIM per second and overall, with the bit rates. It
takes files, or captures both streams from the network for you:

```
python3 /opt/caricoder3/tests/quality.py \
    --source udp://224.100.1.5:49500 --program 1840 \
    --output udp://224.6.7.9:6789 --seconds 20
```

A rendition smaller than the source is compared with the source scaled
to its size. Rough guide for broadcast content: PSNR above 40 dB is
hard to tell from the source, 35-40 good, 30-35 visible on detail,
below 30 clearly degraded; SSIM above 0.95 good.

## Tests

```
make test                         # 79 tests, about ten minutes
python3 tests/run_tests.py --list
python3 tests/run_tests.py live   # only the tests with "live" in the name
python3 tests/run_tests.py xc_    # only the transcoding tests

make fuzz                         # needs clang
build/fuzz_engine -max_len=400000 build/corpus
python3 tests/fuzz_seeds.py build/corpus-live
build/fuzz_live -max_len=400000 build/corpus-live
python3 tests/fuzz_seeds.py --xcode build/corpus-xcode      # needs ffmpeg
LSAN_OPTIONS=suppressions=tests/lsan.supp build/fuzz_xcode -max_len=400000 build/corpus-xcode

python3 tests/av_sync.py in.ts out.ts     # audio sync of a transcode, measured
```

The transcoding tests need ffmpeg and numpy, and skip themselves without.
ffmpeg is used only by the tests: to make real streams, and as the
reference decoder that the output is judged by (does it decode cleanly,
is every picture at its original time, how close is it to the source, and
where does the audio sit).

The checker in `tests/` is a second, independent implementation of the
transport stream standard in Python. It shares nothing with the C code, so
agreement between the two is evidence rather than an echo. Where ffmpeg and
TSDuck are installed, the suite also has them decode and verify the output.

File tests run under AddressSanitizer and UndefinedBehaviorSanitizer. Live
tests run in real time over UDP on the loopback interface.

There are three fuzzers. `fuzz_engine` feeds the pipeline as a file run.
`fuzz_live` feeds it as a live channel, with made-up arrival times, so
that the clock handling is fuzzed too: stalls, bursts, restarts, missing
and damaged PCRs. `fuzz_xcode` runs with audio and video transcoding on,
so that mangled data goes through our framing and into the codec
libraries. All check every output packet for a valid header, an unbroken
continuity counter and a forward-moving PCR, on top of the usual memory
checks. `fuzz_xcode` is a file run on the CPU: it does not reach the
transcoder threads, the live clock handling, or the NVIDIA code.

To check a stream of your own, on any machine with Python 3:

```
python3 tests/ts_check.py out.ts --cbr --against in.ts
```

## Known limits

Transcoding:

- No change of frame rate. The audio sample rate is not changed either.
  Deinterlacing happens only on the way to a scaled rendition, and on the
  CPU it is a plain field blend (soft on motion).
- No AC-3 or HE-AAC output. No E-AC-3 input. MPEG-2 video is decoded
  (CPU or NVDEC) but never produced.
- HE-AAC input needs the full fdk-aac, which the installer builds; a build
  against Ubuntu's package is half band (see "HE-AAC").
- The NVIDIA path: first run on a Tesla P40 on 2026-10-14 (self-test and a
  live HEVC to H.264 channel); the B-frame part came after that run and
  has passed only the stand-in libraries so far. Decoding on the card for
  an encoder on the CPU is not built. Read against the SDK but untested, because the stand-in
  libraries cannot show them: PES packets that hold several pictures (a
  picture waiting to be fetched could be overwritten), the pictures in
  flight at a change of picture size (dropped), and streams that carry no
  frame rate (25 a second is assumed, which skews the bit rate if wrong).
- A change of picture format restarts the encoder with a key frame. An
  interlaced stream that turns progressive is only followed after 500
  progressive pictures in a row, because interlaced streams carry
  progressive pictures among the others; a new colour description or
  field order after 10 in a row.
- In a CBR output with little room to spare, beside a copied video stream
  (which sends its key frames in bursts, as the source did), new audio
  has 75 ms in hand, where a copied audio stream has whatever the source
  gave it. If `late_units` counts up in such a set-up, give the mux more
  room (`-r`). A VBR output has no such contention.
- When the source changes the coding of a stream (new PMT), the input
  waits for the old transcoder to finish: at most 0.3 s for its queue,
  plus the time the encoder takes to give out what it holds (tens of
  milliseconds at the default presets).
- File runs only: with `--video`, a file whose clock starts within about a
  second of zero (files made by ffmpeg do) and a tight CBR rate, the first
  new pictures are due before the output begins and are sent in a bunch;
  copied audio can be up to 0.7 s late for the first second or two.
- When the source's clock steps, units that were already on their way to
  a transcoder with the old clock's time are not moved (a copied stream's
  are): up to 2 s of video or 1 s of audio around the step can be lost.
- MP2 or MP3 made from another coding at 16, 22.05 or 24 kHz is announced
  as stream type 0x03, where 0x04 would be exact (from MPEG audio input
  the type is kept).
- Interlaced video stays interlaced through x264. x265, and NVENC for
  H.265, encode the two fields together as one frame.
- A picture format the encoder cannot take (4:2:2, more than 10 bits)
  cannot be transcoded.
- When the input stops, the pictures the encoder was holding back (up to
  0.7 s with x264) stay inside it until the input returns.
- An AAC output is not announced with a DVB AAC descriptor in the PMT;
  receivers that insist on one may need it added.
- libx265 3.5 does not give back 1 to 5 kB each time an encoder is opened
  (a fault in the library: a bare open and close shows it). That happens
  once per stream, and again only if the picture format changes.
- Not explained: with one particular damaged input, about 2 runs in 100
  end with the memory of one picture (1 MB at 640x360) not freed inside
  libx264. Runs that show it and runs that do not have identical counts
  and no errors, and it is one picture whatever the length of the run,
  which points at the moment the encoder is closed (when a stream ends or
  changes format), not at something that grows while running. Whether the
  fault is in the library or in how it is used here is not known.

Remultiplexing:

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
- A clock step is only visible at a PCR. PES packets that arrive after the
  source's clock has stepped but before its next PCR are taken for the old
  clock's and keep timestamps that are off by the size of the step. With
  PCRs every 40 ms or less that is one or two packets per stream per step.
- A clock step is confirmed in about 0.3 s, or three PCRs if that takes
  longer. If it cannot be confirmed within 0.6 s, the content that has
  arrived by then is sent on the old clock after all. That happens when
  PCRs are more than about 0.3 s apart, or when the source delivers
  faster or slower than real time straight after the step. If the step is
  then confirmed, what is still waiting to be sent is moved onto the new
  clock: that saves it after a step forward of up to 15 s, where it
  leaves late rather than not at all. After a step backward, or a longer
  one forward, that content is lost, as it was before this mechanism
  existed.
- A backlog that is caught up slowly, at less than about 1.2 times real
  time, cannot be told from a clock that has stepped back, and moves the
  time line: once, or in several small steps as it drains.
- While a clock question is open, content is held for up to 0.3 s (0.6 s
  with sparse PCRs). If it turns out to be a backlog after all, that
  content, late already, is that much later. Where it was more than
  about 0.7 s late to begin with, some of it now passes the one second
  limit and is dropped instead of being sent late.
- A damaged PCR that reads ahead by less than 200 ms, or by less than
  three PCR intervals on a stream whose PCRs are further apart than
  70 ms, is acted on until the next PCR: no break and no loss, but the
  packets just before it leave late by up to the size of the error.
- If the PCR stops while a clock question is open and the step in
  question was more than 5 s back or 30 s ahead, the streams that carry
  on are discarded until the PCR returns: their timestamps are too far
  from the only clock there is to be believed.
- Two or more damaged PCRs in a row that agree with each other are
  believed. The service is then disturbed for about as long as the wrong
  value is off by, up to a few seconds.
- If a stream is removed from the PMT while one of its PES packets is
  half sent, that packet is finished first. Not so if its PID is in the
  same moment given to a table or to a stream of another kind: then the
  packet is cut off.
- On loss of input the output keeps running. Going silent after a timeout,
  so that a downstream failover switch can react, is not built yet.
- No PID or service-ID remapping.

See [docs/DESIGN.md](docs/DESIGN.md) for how it works inside.
