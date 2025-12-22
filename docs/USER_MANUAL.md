# CariTranscoder User Manual

This manual provides step-by-step instructions for using CariTranscoder to manage video input streams, preview content, and monitor A/V synchronization.

## Table of Contents

1. [Getting Started](#getting-started)
2. [Managing Input Streams](#managing-input-streams)
3. [Stream Preview](#stream-preview)
4. [Bitrate Monitoring](#bitrate-monitoring)
5. [A/V Sync Monitoring](#av-sync-monitoring)
6. [Troubleshooting](#troubleshooting)

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
   - **Address**: SRT server address (e.g., `srt.server.com:9000`)
   - **Mode**: Connection mode (Caller, Listener, Rendezvous)
   - **Latency**: Buffer latency in milliseconds (default: 200)
   - **Stream ID**: Optional identifier for multi-stream servers
   - **Passphrase**: Optional encryption passphrase
   - **Key Length**: Encryption key length (Auto, AES-128, AES-192, AES-256)

   **Source Settings (for HLS):**
   - **URL**: Full HLS playlist URL (e.g., `https://example.com/stream.m3u8`)
   - **Live Mode**: Enable for live streams (default: Yes)
   - **Bitrate Selection**: Auto, Highest, Lowest, or specify Max/Min value
   - **Bitrate Value**: Target bitrate in kbps (for Max/Min modes)
   - **Resolution**: Auto, Highest, or Lowest resolution variant

   **Source Settings (for HTTP):**
   - **URL**: Direct HTTP URL to MPEG-TS stream (e.g., `http://server:port/path/mpegts`)
   - HTTP input is simpler than HLS - it receives MPEG-TS directly over HTTP
   - No bitrate or resolution selection needed
   - Uses TSDuck's HTTP plugin for reliable transport stream reception

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
