# CariTranscoder User Manual

This manual provides step-by-step instructions for using CariTranscoder to manage video input streams, preview content, and monitor A/V synchronization.

## Table of Contents

1. [Getting Started](#getting-started)
2. [Managing Input Streams](#managing-input-streams)
3. [Video Transcoding](#video-transcoding)
4. [Stream Preview](#stream-preview)
5. [Bitrate Monitoring](#bitrate-monitoring)
6. [A/V Sync Monitoring](#av-sync-monitoring)
7. [Troubleshooting](#troubleshooting)
8. [Port Reference](#port-reference)
9. [Best Practices](#best-practices)

---

## Getting Started

### Accessing the Web Interface

1. Open your web browser
2. Navigate to `http://your-server:8080`
3. Log in with your credentials (default: `admin` / `admin`)

### Dashboard Overview

The dashboard provides an overview of all configured inputs, transcoders, and outputs. The main navigation includes:

- **Inputs** - Manage source streams
- **Transcoders** - Configure encoding profiles
- **Muxers** - Set up MPTS multiplexing
- **Outputs** - Configure output destinations
- **Settings** - System configuration

---

## Managing Input Streams

### Adding a New Input

1. Navigate to **Inputs** page
2. Click **Add Input** button
3. Fill in the input configuration:

   **Basic Settings:**
   - **Name**: A unique identifier for the input (e.g., "channel1", "bet", "rai")
   - **Type**: Select the input type (UDP, SRT, RTMP, HLS, HTTP, RIST)

   **Source Settings (for UDP/Multicast):**
   - **Address**: Multicast address (e.g., `239.100.0.1`)
   - **Port**: UDP port (e.g., `10000`)
   - **Interface**: Network interface (optional)

   **Source Settings (for SRT):**
   - **Address**: SRT server address (e.g., `192.168.1.100` or `srt.server.com`)
   - **Port**: SRT port (e.g., `9000`)
   - **Mode**: Connection mode:
     - **Caller**: Connects to a remote SRT listener (most common)
     - **Listener**: Waits for incoming SRT connections
     - **Rendezvous**: Both sides connect simultaneously
   - **Latency**: Buffer latency in milliseconds (default: 120)
     - Higher values improve reliability on lossy networks
     - Lower values reduce delay but may cause drops
   - **Stream ID**: Optional identifier for multi-stream servers
     - Used by server to route to correct stream
     - Example: `#!::r=channelname` (Haivision format)
   - **Passphrase**: Optional AES encryption passphrase (10-79 characters)
   - **Key Length**: Encryption key length (0=disabled, 16=AES-128, 24=AES-192, 32=AES-256)

   **SRT Technical Details:**
   - Uses `srt-live-transmit` for reliable stream reception
   - Stats collected via named pipe to avoid file growth issues
   - Live statistics available via `/srt-stats` API endpoint
   - Automatic reconnection on connection loss

   **Source Settings (for HLS):**
   - **URL**: Full HLS playlist URL (e.g., `https://example.com/stream.m3u8`)
   - HLS streams are received via ffmpeg for reliable playback
   - ffmpeg remuxes the stream with automatic PID assignment (video=256, audio=257)
   - Use the **Scan** button to discover PIDs before saving

   **Source Settings (for HTTP):**
   - **URL**: Direct HTTP URL to MPEG-TS stream (e.g., `http://server:port/path/mpegts`)
   - HTTP input is simpler than HLS - it receives MPEG-TS directly over HTTP
   - No bitrate or resolution selection needed
   - Uses TSDuck's HTTP plugin for reliable transport stream reception

   **Source Settings (for RIST):**
   - **Address**: RIST server address (e.g., `239.0.0.1:5000` or `server.com:5000`)
   - **Profile**: RIST profile - Simple, Main (default), or Advanced
   - **Buffer**: Buffer size in milliseconds for retransmissions (0 = disabled for low latency)
   - **Encryption**: Encryption type - Disabled, AES-128, or AES-256
   - **Secret**: Encryption passphrase (required if encryption is enabled)
   - RIST provides reliable transport with automatic retransmission of lost packets

   **PID Configuration:**
   - **Video PID**: The MPEG-TS PID for video (e.g., `211`)
   - **Audio PID**: The MPEG-TS PID for audio (e.g., `221`)

4. Click **Save** to create the input

### Input Configuration File Format

Input configurations are stored in `/etc/caritrans/inputs/`. Example configuration:

```ini
[source]
name = bet
type = udp
address = 239.100.0.1
port = 10000
video_pid = 211
audio_pid = 221

[output]
type = udp
address = 239.100.0.1
port = 10000
```

### Starting and Stopping Inputs

- Click the **Play** button to start an input
- Click the **Stop** button to stop an input
- The status dot indicates the current state:
  - **Green (pulsing)**: Running and receiving data
  - **Red**: Running but no data received
  - **Gray**: Stopped

---

## Video Transcoding

The `cari-transcoder` tool provides real-time video and audio transcoding using GStreamer. It receives MPEG-TS streams via UDP multicast, transcodes video/audio, and outputs to TCP or stdout.

### Quick Start

```bash
# Detect input stream format
cari-transcoder --input 239.100.0.1:5000 --detect-only

# Basic transcode to H.264 at 2 Mbps
cari-transcoder --input 239.100.0.1:5000 --video-bitrate 2000000 --tcp-port 8888

# View output with ffplay
ffplay tcp://localhost:8888
```

### Processing Modes

Both video and audio support three processing modes:

| Mode | Description |
|------|-------------|
| **transcode** | Decode and re-encode (default) |
| **passthrough** | Copy stream without modification |
| **drop** | Discard the stream entirely |

### Video Encoding

#### Supported Output Codecs

| Codec | Option | Encoder | Notes |
|-------|--------|---------|-------|
| H.264/AVC | `--video-codec h264` | x264enc | Full x264 options support |
| H.265/HEVC | `--video-codec h265` | x265enc | Bandwidth efficient |
| MPEG-2 | `--video-codec mpeg2` | avenc_mpeg2video | Legacy compatibility |

#### Common Video Options

| Option | Description | Default |
|--------|-------------|---------|
| `--video-bitrate BPS` | Target bitrate in bits/second | 5000000 (5 Mbps) |
| `--video-preset PRESET` | Encoder speed/quality tradeoff | superfast |
| `--keyframe-interval N` | GOP size in frames | 60 |

**Preset Options** (fastest to slowest): ultrafast, superfast, veryfast, faster, fast, medium, slow, slower, veryslow

#### x264 Encoder Options

For H.264 encoding, additional x264-specific options are available:

| Option | Description | Default |
|--------|-------------|---------|
| `--profile` | H.264 profile (baseline/main/high) | main |
| `--bframes N` | B-frames between I and P frames (0-16) | 0 |
| `--ref N` | Number of reference frames (1-12) | 1 |
| `--qp-min N` | Minimum quantizer (0-51) | 10 |
| `--qp-max N` | Maximum quantizer (0-51) | 51 |
| `--vbv-bufsize MS` | VBV buffer size in milliseconds | 600 |
| `--threads N` | Encoding threads (0=auto) | 0 |
| `--sliced-threads` | Enable low-latency sliced threading | enabled |
| `--cabac` / `--no-cabac` | CABAC entropy coding | enabled |
| `--trellis` | Trellis quantization | disabled |
| `--aud` / `--no-aud` | Access Unit delimiters | enabled |
| `--psy-tune TUNE` | Psychovisual tuning (film/animation/grain) | none |
| `--x264-opts STRING` | Custom x264 options (key=val:key=val) | none |

### Video Scaling

Scale video to different resolutions:

```bash
# Scale to 720p
cari-transcoder --input 239.100.0.1:5000 --scale 1280x720 --video-bitrate 2000000

# Scale to 480p with lanczos algorithm
cari-transcoder --input 239.100.0.1:5000 --scale 854x480 --scale-method 3
```

| Option | Description | Default |
|--------|-------------|---------|
| `--scale WxH` | Output resolution (e.g., 1280x720) | none |
| `--scale-method N` | Scaling algorithm (see below) | 1 (bilinear) |
| `--add-borders` | Add black bars to preserve aspect ratio | disabled |
| `--scale-threads N` | Scaling threads (0=auto) | 0 |
| `--deinterlace` | Enable deinterlacing | disabled |

**Scale Methods:**
- 0 = Nearest neighbor (fastest, blocky)
- 1 = Bilinear (default, good balance)
- 2 = 4-tap
- 3 = Lanczos (high quality)
- 4 = Bilinear2
- 5 = Sinc
- 6 = Hermite
- 7 = Spline
- 8 = Catmull-Rom
- 9 = Mitchell

### Audio Encoding

#### Supported Output Codecs

| Codec | Option | Encoder |
|-------|--------|---------|
| AAC | `--audio-codec aac` | avenc_aac (default) |
| AC3 | `--audio-codec ac3` | avenc_ac3 |
| MP2 | `--audio-codec mp2` | avenc_mp2 |

#### Common Audio Options

| Option | Description | Default |
|--------|-------------|---------|
| `--audio-bitrate BPS` | Audio bitrate in bits/second | 128000 |
| `--audio-channels N` | Number of channels (1, 2, 6) | 2 |
| `--audio-samplerate HZ` | Sample rate in Hz | 48000 |

#### AAC Encoder Options

| Option | Description | Default |
|--------|-------------|---------|
| `--aac-coder CODER` | Coding algorithm (anmr/twoloop/fast) | fast |
| `--aac-is` / `--no-aac-is` | Intensity stereo coding | enabled |
| `--aac-ms` / `--no-aac-ms` | M/S stereo coding | enabled |
| `--aac-pns` / `--no-aac-pns` | Perceptual noise substitution | enabled |
| `--aac-tns` / `--no-aac-tns` | Temporal noise shaping | enabled |
| `--aac-ltp` / `--no-aac-ltp` | Long term prediction | disabled |
| `--aac-cutoff HZ` | Audio cutoff bandwidth (0=auto) | 0 |

### Output Options

| Option | Description |
|--------|-------------|
| `--tcp-port PORT` | Output to TCP server on specified port (default: 8888) |
| `--stdout` | Output to stdout for piping to tsp |

### Example Workflows

**Basic SD Transcode:**
```bash
cari-transcoder --input 239.100.0.1:5000 \
    --video-bitrate 2000000 \
    --audio-bitrate 128000 \
    --tcp-port 8888
```

**HD to SD Downscale:**
```bash
cari-transcoder --input 239.100.0.1:5000 \
    --scale 854x480 --deinterlace \
    --video-bitrate 1500000 \
    --tcp-port 8888
```

**H.265 Encoding for Bandwidth Savings:**
```bash
cari-transcoder --input 239.100.0.1:5000 \
    --video-codec h265 \
    --video-bitrate 1500000 \
    --tcp-port 8888
```

**High-Quality Broadcast:**
```bash
cari-transcoder --input 239.100.0.1:5000 \
    --video-codec h264 --video-bitrate 8000000 \
    --video-preset medium --profile high \
    --bframes 2 --ref 3 \
    --audio-codec aac --audio-bitrate 192000 \
    --tcp-port 8888
```

**Pipe to TSDuck for Multicast Output:**
```bash
cari-transcoder --input 239.100.0.1:5000 \
    --video-bitrate 3000000 --stdout | \
    tsp -I file - -P regulate --bitrate 5000000 -O ip 239.100.0.2:5000
```

### Troubleshooting Transcoder

**No video playing (audio only):**
- Check video codec compatibility
- For H.265, ensure `h265parse config-interval=-1` is in pipeline
- Test with `GST_DEBUG=2` environment variable

**Stream detection fails:**
```bash
# Verify input stream is accessible
ffprobe udp://@239.100.0.1:5000

# Check with more analysis time
ffprobe -analyzeduration 10000000 udp://@239.100.0.1:5000
```

**High CPU usage:**
- Use faster preset: `--video-preset ultrafast`
- Reduce resolution: `--scale 854x480`
- Lower bitrate: `--video-bitrate 1000000`

**Output stuttering:**
- Increase VBV buffer: `--vbv-bufsize 1000`
- Check network bandwidth to destination
- Use `--sliced-threads` for lower latency

---

## Stream Preview

### Opening the Preview Modal

1. Navigate to the **Inputs** page
2. Click the **Preview** button (eye icon) for any input
3. The preview modal will open with:
   - Live video player
   - Stream information panel
   - Bitrate graph
   - A/V sync graph

### Preview Modal Features

The preview modal displays:

**Video Player (Left Panel)**
- Live HLS playback of the stream
- Standard video controls (play, pause, volume, fullscreen)

**Stream Info (Right Panel)**
- Video codec and profile
- Resolution and frame rate
- Audio codec and channel configuration
- Language tags

**Bitrate Monitor**
- Real-time graph showing video (blue) and audio (green) bitrate
- Updates every 5 seconds
- Shows 1 hour of history

**A/V Sync Monitor**
- Current A→V and V→A offset values
- Status indicator (OK/WARNING/ERROR)
- 24-hour history graph
- Updates every 5 minutes

**RIST Statistics (RIST inputs only)**
- Link quality percentage with color-coded status (green ≥99%, yellow ≥95%, red <95%)
- Connected peers count
- Round-trip time (RTT) in milliseconds
- Retry bandwidth overhead
- Packet statistics: received, missing, recovered, lost, reordered, 1st retry
- Updates every 5 seconds

**SRT Statistics (SRT inputs only)**
- Round-trip time (RTT) in milliseconds - connection latency indicator
- Estimated bandwidth in Mbps - available link capacity
- Packet statistics:
  - Packets received and sent
  - Packets lost in transit
  - Packets dropped (arrived too late)
  - Packets retransmitted (recovered)
- Byte counters for total data transferred
- Updates every ~1 second

---

## Bitrate Monitoring

### Understanding Bitrate Display

The bitrate monitor shows:

- **Video Bitrate** (Blue): Current video stream bitrate in Mbps
- **Audio Bitrate** (Green): Current audio stream bitrate in Kbps

### Reading the Graph

- X-axis: Time (HH:MM:SS format)
- Y-axis: Bitrate (automatically scaled)
- Hover over the graph to see exact values at any point

### Interpreting Bitrate Data

| Metric | Healthy Range | Notes |
|--------|---------------|-------|
| Video Bitrate | 1-8 Mbps (SD), 4-20 Mbps (HD) | Should be stable |
| Audio Bitrate | 64-384 Kbps | Should be constant |
| Variation | < 10% | Large swings indicate issues |

---

## A/V Sync Monitoring

### Understanding A/V Sync Values

The A/V sync monitor measures the gap between audio and video presentation timestamps:

- **A→V Mean**: Average gap from audio frame to next video frame
- **V→A Mean**: Average gap from video frame to next audio frame

### Status Thresholds

| Status | A→V Offset | Action |
|--------|------------|--------|
| **OK** (Green) | < 300 ms | Normal operation |
| **WARNING** (Orange) | 300-600 ms | Monitor closely, investigate if persistent |
| **ERROR** (Red) | > 600 ms | Investigate immediately |

### How A/V Sync is Measured

1. The system captures 10 seconds of PTS (Presentation Time Stamp) data
2. Entries are sorted by presentation time
3. Consecutive same-PID frames are filtered out
4. Gaps between alternating audio/video frames are calculated
5. The mean offset is reported (maximum value excluded to filter capture errors)

### Reading the A/V Sync Graph

- **Purple Line (A→V)**: Audio to video gap over time
- **Cyan Line (V→A)**: Video to audio gap over time
- The graph shows 24 hours of history (288 samples at 5-minute intervals)

### What Healthy A/V Sync Looks Like

**Good Indicators:**
- Flat, stable lines with minimal variation
- A→V + V→A roughly equals the frame interval
- No upward or downward drift over time

**Warning Signs:**
- Increasing values over time (indicates drift)
- Large sudden changes (indicates stream issues)
- Values exceeding 300ms

---

## Troubleshooting

### Preview Not Loading

1. **Check if input is running**
   ```bash
   ps aux | grep udp_input
   ```

2. **Verify stream is accessible**
   ```bash
   ffprobe udp://239.100.0.1:10000
   ```

3. **Check preview process**
   ```bash
   ps aux | grep player_preview
   ```

4. **Check HLS segments**
   ```bash
   ls -la /var/www/caritrans/public/preview/INPUT_NAME/
   ```

### A/V Sync Monitor Shows "Error"

1. **Check if cari-avsync service is running**
   ```bash
   systemctl status cari-avsync
   ```

2. **Restart the service**
   ```bash
   sudo systemctl restart cari-avsync
   ```

3. **Check service logs**
   ```bash
   journalctl -u cari-avsync -f
   ```

4. **Test API manually**
   ```bash
   curl http://localhost:8082/status
   ```

### High A/V Sync Values

If A/V sync values are consistently high:

1. **Check source encoder settings**
   - Verify encoder is outputting properly timestamped streams
   - Check for keyframe alignment issues

2. **Check network path**
   - Look for packet loss or jitter
   - Verify multicast routing

3. **Check PID configuration**
   - Ensure video_pid and audio_pid match actual stream PIDs
   - Use `tsp -I ip 239.x.x.x:port -P analyze -O drop` to verify PIDs

### Bitrate Graph Flat at Zero

1. **Verify input is receiving data**
   - Check udp_input process is running
   - Verify network connectivity

2. **Check metrics API**
   ```bash
   curl http://localhost:API_PORT/metrics
   ```

3. **Restart the input**
   - Stop and restart the input from the web interface

### SRT Input Not Connecting

1. **Check service status**
   ```bash
   systemctl status cari-srt-INPUTNAME
   journalctl -u cari-srt-INPUTNAME -f
   ```

2. **Test SRT connection manually**
   ```bash
   srt-live-transmit 'srt://ADDRESS:PORT?mode=caller' file://con | hexdump -C | head
   ```

3. **Verify Stream ID format**
   - Some servers require specific Stream ID formats
   - Common format: `#!::r=streamname`
   - Check server documentation for requirements

4. **Check firewall/network**
   - SRT uses UDP on the configured port
   - Ensure firewall allows UDP traffic

### SRT Stats Not Showing

1. **Check stats file exists**
   ```bash
   ls -la /tmp/srt-input-*-stats.json
   cat /tmp/srt-input-INPUTNAME-stats.json
   ```

2. **Check stats pipe exists**
   ```bash
   ls -la /tmp/srt-input-*-stats.pipe
   ```

3. **Test stats API endpoint**
   ```bash
   curl http://localhost:API_PORT/srt-stats
   ```

4. **Check srt_input logs**
   ```bash
   journalctl -u cari-srt-INPUTNAME | grep -i stats
   ```

---

## Command Reference

### Service Management

```bash
# View all CariTranscoder services
systemctl list-units | grep cari

# Restart main services
sudo systemctl restart cari-api
sudo systemctl restart cari-avsync
sudo systemctl restart nginx

# View logs
journalctl -u cari-api -f
journalctl -u cari-avsync -f
```

### API Endpoints

```bash
# Check A/V sync for all inputs
curl http://localhost:8082/status

# Check A/V sync for specific input
curl http://localhost:8082/status/INPUT_NAME

# Get A/V sync history
curl http://localhost:8082/history/INPUT_NAME

# Health check
curl http://localhost:8082/health
```

### Configuration Files

| Path | Description |
|------|-------------|
| `/etc/caritrans/inputs/` | Input configurations |
| `/etc/caritrans/transcoders/` | Transcoder configurations |
| `/etc/caritrans/outputs/` | Output configurations |
| `/var/log/caritrans/` | Log files |
| `/var/lib/caritrans/` | Data files |

---

## Port Reference

CariTranscoder uses several ports. Each input has a base `api_port` (assigned during creation) with additional ports derived from it.

### System Ports

| Port | Service | Description |
|------|---------|-------------|
| 8080 | Web UI | Nginx web interface |
| 8000 | CariTrans API | FastAPI for privileged operations |
| 8082 | A/V Sync | cari-avsync monitor service |

### Per-Input Ports

| Offset | Port Example | Service |
|--------|--------------|---------|
| +0 | 9100 | Input API (health, metrics, history) |
| +1000 | 10100 | Player Preview (HLS generation) |
| +2000 | 11100 | RIST Metrics (RIST inputs only) |

**Example:** If your input has `api_port=9105`:
- Input API: `http://localhost:9105/metrics`
- Preview API: `http://localhost:10105/status`
- RIST Stats: `http://localhost:9105/rist-stats` (fetches from port 11105 internally)

### Checking Port Usage

```bash
# See all ports in use by CariTranscoder
ss -tlnp | grep -E '(8000|8080|8082|9[0-9]{3}|1[01][0-9]{3})'
```

---

## Best Practices

### Input Configuration

1. Always specify both video and audio PIDs explicitly
2. Use descriptive, lowercase names without spaces
3. Configure output address for monitoring features

### Monitoring

1. Keep the preview modal open during critical broadcasts for real-time monitoring
2. Check A/V sync regularly - stable values indicate healthy streams
3. Set up alerts based on A/V sync thresholds if using external monitoring

### Maintenance

1. Regularly check logs for errors
2. Monitor disk space for HLS segment storage
3. Restart services after system updates

---

*Last updated: December 2024*
