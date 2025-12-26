# CariTranscoder

A professional video transcoding and streaming appliance for broadcast and IPTV applications. CariTranscoder provides a web-based interface for managing video inputs, transcoders, multiplexers, and outputs with real-time monitoring and stream preview capabilities.

## Features

- **Multi-Input Support**: UDP/Multicast, SRT, HLS, RTMP, RIST, and file-based inputs
- **Hardware Transcoding**: NVIDIA NVENC, Intel QuickSync, and software encoding
- **Stream Multiplexing**: Combine multiple video/audio streams into MPTS
- **Multiple Outputs**: UDP, SRT, RTMP, HLS output support
- **Web Preview**: Live HLS preview with stream analysis (ffprobe integration)
- **Real-time Monitoring**: Bitrate graphs, PID statistics, and stream health
- **REST API**: FastAPI-based privileged operations service
- **Appliance Mode**: Designed for turnkey deployment
- **A/V Sync Monitor**: Real-time audio/video synchronization monitoring with 24-hour trending

## Documentation

- **[User Manual](docs/USER_MANUAL.md)** - Complete guide to using CariTranscoder

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                      Web Interface (PHP)                        │
│                     http://server:8080                          │
└─────────────────────────────────────────────────────────────────┘
                                │
                ┌───────────────┼───────────────┐
                ▼               ▼               ▼
        ┌───────────┐   ┌───────────┐   ┌───────────┐
        │  Inputs   │   │Transcoders│   │  Outputs  │
        │(udp_input)│   │(GStreamer)│   │   (tsp)   │
        └───────────┘   └───────────┘   └───────────┘
                │               │               │
                └───────────────┼───────────────┘
                                ▼
                    ┌───────────────────┐
                    │   CariTrans API   │
                    │ (FastAPI Service) │
                    │    Port 8000      │
                    └───────────────────┘
```

## Components

| Component | Description |
|-----------|-------------|
| `cari-input` | Input stream handler with UDP/SRT/HLS/RTMP support |
| `cari-transcoder` | GStreamer-based video transcoding |
| `cari-mux` | MPEG-TS multiplexer for combining streams |
| `cari-output` | Output stream distribution |
| `cari-stats` | Real-time statistics collection |
| `cari-api` | FastAPI service for privileged operations |
| `cari-avsync` | A/V sync monitor with 24-hour trending |
| `udp_input` | UDP input tool with PID filtering and monitoring |
| `srt_input` | SRT input tool with caller/listener modes and encryption |
| `rist_input` | RIST input tool with Simple/Main/Advanced profiles and encryption |
| `hls_input` | HLS input tool using ffmpeg for reliable stream reception |
| `player_preview` | HLS preview generator using FFmpeg |

## Requirements

- Ubuntu 20.04+ or Debian 11+
- GStreamer 1.0 with plugins (base, good, bad, ugly)
- FFmpeg
- TSDuck
- Nginx with PHP-FPM
- Python 3.8+ with FastAPI/Uvicorn
- libmicrohttpd (for REST API in C tools)

## Installation

### Fresh Install

Run the install script with curl:

```bash
curl -sSL "https://raw.githubusercontent.com/caritechsolutions/Caricoder2/claude/video-transcoder-gstreamer-YnBIH/scripts/install.sh?$(date +%s)" | sudo bash
```

This will:
- Install all dependencies (GStreamer, FFmpeg, TSDuck, Nginx, PHP, etc.)
- Create the `caritrans` system user
- Set up directory structure
- Build and install all binaries
- Configure Nginx and PHP-FPM
- Set up systemd services

### Update Existing Installation

To update an existing installation while preserving your configuration:

```bash
curl -sSL "https://raw.githubusercontent.com/caritechsolutions/Caricoder2/claude/video-transcoder-gstreamer-YnBIH/scripts/update.sh?$(date +%s)" | sudo bash -s -- -y
```

The update script will:
- Back up current code to `/tmp/caritrans_backup_*`
- Update web interface files
- Rebuild tools (udp_input, player_preview)
- Update the FastAPI service
- Restart necessary services

**Preserved during update:**
- `/etc/caritrans/*` (configurations)
- `/var/log/caritrans/*` (log files)
- `/var/lib/caritrans/*` (data files)
- Running service states

## Directory Structure

```
/etc/caritrans/           # Configuration files
├── inputs/               # Input configurations
├── transcoders/          # Transcoder configurations
├── muxers/               # Muxer configurations
├── outputs/              # Output configurations
└── users.conf            # Web UI user credentials

/var/www/caritrans/       # Web interface
├── public/               # Web root (nginx document root)
├── includes/             # PHP includes
└── templates/            # PHP templates

/var/log/caritrans/       # Log files
/var/lib/caritrans/       # Data files
/usr/local/bin/           # Compiled binaries
```

## Web Interface

Access the web interface at `http://your-server:8080`

Default credentials: `admin` / `admin`

### Pages

- **Dashboard**: Overview of all inputs, transcoders, and outputs
- **Inputs**: Configure and monitor input streams
- **Transcoders**: Set up video/audio transcoding profiles
- **Muxers**: Configure MPTS multiplexing
- **Outputs**: Manage output destinations
- **Settings**: System configuration

### Stream Preview

Click on any input to open the preview modal:
- **Live HLS video player** - Real-time stream playback
- **Stream information** - Video/audio codecs, resolution, frame rate, audio channels
- **Bitrate Monitor** - Real-time graph showing video and audio bitrate (updates every 5 seconds)
- **A/V Sync Monitor** - Shows audio-to-video and video-to-audio gap measurements with 24-hour history graph (updates every 5 minutes)

The preview modal features a modern gradient design with status indicators and detailed stream analysis.

## API Endpoints

### CariTrans API (Port 8000)

```bash
# Health check
curl http://localhost:8000/health

# Start preview
curl -X POST http://localhost:8000/preview/start \
  -H "Content-Type: application/json" \
  -d '{"input_id": "input-001", "multicast_addr": "239.1.1.1", "port": 5000}'

# Stop preview
curl -X POST http://localhost:8000/preview/stop \
  -H "Content-Type: application/json" \
  -d '{"input_id": "input-001"}'

# Scan stream
curl -X POST http://localhost:8000/stream/scan \
  -H "Content-Type: application/json" \
  -d '{"address": "239.1.1.1", "port": 5000, "program": 1}'
```

### Player Preview API (Dynamic Port)

```bash
# Health check
curl http://localhost:PORT/health

# Keep alive (call every 30s)
curl -X POST http://localhost:PORT/keepalive

# Get status
curl http://localhost:PORT/status
```

### A/V Sync Monitor API (Port 8082)

The cari-avsync service monitors audio/video synchronization for all running inputs. It captures PTS values, sorts by presentation time, filters to alternating A/V frames, and calculates the gap between consecutive audio and video frames.

**How it works:**
1. Captures 10 seconds of PTS data using `tsp pcrextract`
2. Sorts entries by PTS value (chronological presentation order)
3. Filters to keep only alternating video/audio frames
4. Calculates A→V gaps (audio to next video) and V→A gaps (video to next audio)
5. Reports mean offset (excluding max value to filter capture errors)
6. Polls every 5 minutes, keeps 24 hours of history (288 samples)

```bash
# Health check
curl http://localhost:8082/health

# Get status of all inputs
curl http://localhost:8082/status

# Get status of single input
curl http://localhost:8082/status/bet

# Get status with 24-hour history
curl http://localhost:8082/history/bet
```

**Status Thresholds:**
| Status | A→V Mean Offset | Description |
|--------|-----------------|-------------|
| OK | < 300 ms | Sync within acceptable range |
| WARNING | 300-600 ms | Sync degraded, investigate |
| ERROR | > 600 ms | Sync out of spec, action needed |

**Example Response:**
```json
{
  "id": "bet",
  "name": "bet",
  "type": "udp",
  "address": "239.100.0.1:10000",
  "video_pid": 211,
  "audio_pid": 221,
  "running": true,
  "current": {
    "timestamp": "2024-12-20T12:05:58Z",
    "a2v_avg_ms": 45.2,
    "v2a_avg_ms": 38.7,
    "a2v_count": 42,
    "v2a_count": 41,
    "status": "OK"
  }
}
```

## Troubleshooting

### Preview not working

1. Check if the stream is accessible:
   ```bash
   ffprobe udp://239.1.1.1:5000
   ```

2. Check player_preview process:
   ```bash
   ps aux | grep player_preview
   ```

3. Check HLS files are being created:
   ```bash
   ls -la /var/www/caritrans/public/preview/input-001/
   ```

4. Check nginx preview location:
   ```bash
   grep -A5 "/preview" /etc/nginx/sites-enabled/caritrans
   ```

### Service issues

```bash
# Check API service
systemctl status cari-api

# View API logs
journalctl -u cari-api -f

# Restart services
systemctl restart nginx php7.4-fpm cari-api
```

## Development

### Building from source

```bash
# Clone repository
git clone https://github.com/caritechsolutions/Caricoder2.git
cd Caricoder2

# Build tools
cd tools/udp_input && make && sudo make install
cd ../player_preview && make && sudo make install
```

### Branch

Current development branch: `claude/video-transcoder-gstreamer-YnBIH`

## Roadmap / TODO

### Input Types
Building on the UDP input foundation:

- [x] **UDP Input** - Multicast/unicast with PID filtering and real-time monitoring
- [x] **SRT Input** - Secure Reliable Transport with caller/listener/rendezvous modes, encryption, streamid
- [x] **RIST Input** - Reliable Internet Stream Transport with Simple/Main/Advanced profiles, encryption, buffer control
- [x] **HLS Input** - HTTP Live Streaming via ffmpeg with automatic PID discovery

### Planned Features
- [x] Web UI for input configuration wizard with type-specific options
- [ ] Transcoder profiles management
- [ ] Output multiplexing configuration
- [ ] System settings page
- [ ] User authentication improvements

## Changelog

### 2024-12-26

**RIST Input Support**
- New `rist_input` tool using vendored librist with ristreceiver
- Supports Simple, Main, and Advanced RIST profiles
- AES-128/AES-256 encryption with secret passphrase
- Configurable buffer size (default: disabled for low latency)
- `/metrics` and `/metrics/history` REST endpoints for GUI bitrate charts
- Full GUI support with RIST-specific options in web interface
- Systemd service generation via Python API
- A/V sync monitoring support for RIST inputs

**A/V Sync Monitor Fixes**
- Fixed service detection for RIST and other inputs with spaces/special chars
- Added `sanitize_to_id()` function to match systemd service naming
- Fixed timezone mismatch between A/V sync and bandwidth graph
- Both now display timestamps in browser-local timezone

**HLS Input Rewrite**
- Replaced TSDuck HLS plugin with ffmpeg for improved stability
- Uses `tsp -I fork` with `ffmpeg -re -i URL -c copy -f mpegts pipe:1`
- ffmpeg handles HLS edge cases more reliably (chunked transfers, redirects, authentication)
- Automatic PID discovery via ffmpeg remuxing (video=256, audio=257, etc.)
- Scanning updated to use ffmpeg for consistent PID detection
- Same architecture as RIST input for maintainability

### 2024-12-22

**HLS Input Support (Initial - replaced by ffmpeg in 2024-12-26)**
- New `hls_input` tool using TSDuck HLS plugin
- Supports live mode (--live) for live HLS streams
- Bitrate selection: auto, highest, lowest, max/min with value
- Resolution selection: auto, highest, lowest
- Full GUI support with HLS-specific options in web interface
- Systemd service generation via Python API
- Service control (start/stop/status) through PHP API
- A/V sync monitoring support for HLS inputs

**SRT Input Fixes**
- Fixed `--transtype live --messageapi` options for proper SRT reception
- Fixed log parsing for bitrate_monitor output format
- Fixed service control from GUI (start/stop buttons now work)
- Fixed service status detection in cari-avsync

### 2024-12-20

**A/V Sync Monitor (cari-avsync)**
- New standalone service for monitoring audio/video synchronization
- Captures PTS values using TSDuck `tsp pcrextract`
- Sorts by PTS, filters alternating A/V frames, calculates gap statistics
- Reports A→V and V→A mean offsets (excludes max to filter capture errors)
- Status thresholds: OK (<300ms), WARNING (300-600ms), ERROR (>600ms)
- Polls every 5 minutes, keeps 24 hours of trending data (288 samples)
- REST API on port 8082 for status queries and history
- Auto-discovers running inputs from `/etc/caritrans/inputs/*.conf`
- Monitors output multicast address from `[output]` config section
- Supports UDP, SRT, RIST, HLS input types

**Input Handling**
- Fixed PMT PID detection for program selection
- Improved stream scanning with proper program association
- Added support for multiple audio PIDs in source configuration

**Player Preview**
- FFmpeg-based HLS preview generation
- Automatic cleanup on keepalive timeout
- Stream analysis via ffprobe integration

## License

Copyright (c) 2024 CariTech Solutions. All rights reserved.

## Support

For issues and feature requests, please use the GitHub issue tracker.
