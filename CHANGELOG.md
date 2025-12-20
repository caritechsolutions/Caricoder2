# Changelog

All notable changes to CariTranscoder are documented in this file.

## [Unreleased] - 2024-12-19

### Major Features

#### Web Preview with HLS Player
- Added live stream preview directly in the web interface
- HLS.js-based video player with adaptive streaming support
- Stream information panel showing codec details via ffprobe
- Keepalive mechanism for automatic cleanup of inactive previews

#### FFmpeg-based HLS Generation
- Replaced TSDuck (tsp) with FFmpeg for HLS segment generation
- Proper keyframe alignment ensures all segments are independently decodable
- H.264 SPS/PPS headers correctly inserted in each segment
- Supports various audio codecs including MP2, AAC, and AC3

#### FastAPI Service (cari-api)
- New Python FastAPI service for privileged operations
- Runs on port 8000 with systemd management
- Handles preview start/stop without requiring PHP to run as root
- Stream scanning API with program-based PID filtering

#### Stream Scanning
- Program-aware stream analysis
- Automatic PID detection for video, audio, and subtitle streams
- PMT PID-based program selection for multi-program transport streams
- Integration with input configuration forms

### Improvements

#### UDP Input Tool
- Rewrote udp_input to spawn tsp for PID filtering and monitoring
- Added `--program` option for program-specific PID selection
- Child process management with proper cleanup
- REST API for status and control

#### Player Preview Tool
- FFmpeg-based HLS generation with `independent_segments` flag
- Automatic segment cleanup with `delete_segments` flag
- Configurable segment duration and live segment count
- Keepalive timeout mechanism (60 seconds default)
- REST API endpoints: `/health`, `/keepalive`, `/status`

#### Web Interface
- Modernized inputs table with gradient header styling
- Real-time bitrate graphs with historical data
- Input preview modal with video player and stream info
- Improved error handling throughout

### Bug Fixes

- **PMT PID Selection**: Fixed program select to use PMT PID instead of program number for tsp compatibility
- **Stream Scan Timeout**: Increased timeout for reliable program detection
- **Preview Directory**: Fixed output directory to use dynamic web root path
- **libmicrohttpd Compatibility**: Fixed MHD_Result type for older library versions
- **Error Suppression**: Fixed PHP error handler to respect @ suppression operator
- **Service Generation**: Fixed UDP input service file generation and start/stop

### Technical Changes

#### Architecture
- Separated privileged operations into FastAPI service
- PHP-FPM runs as www-data with sudoers for specific commands
- Nginx configured with `/preview` location for HLS streams
- Systemd service templates for all components

#### Dependencies
- Added FastAPI and Uvicorn for Python API
- libmicrohttpd for C-based REST APIs
- HLS.js for web video playback
- FFmpeg for stream processing and HLS generation

### File Changes

#### New Files
- `api/main.py` - FastAPI service
- `api/cari-api.service` - Systemd service file
- `tools/player_preview/player_preview.c` - FFmpeg-based HLS generator
- `tools/player_preview/old_player.c` - Backup of tsp-based version
- `tools/udp_input/udp_input.c` - UDP input with PID filtering

#### Modified Files
- `web/public/inputs.php` - Added preview modal and HLS player
- `web/includes/functions.php` - Preview and scan functions
- `scripts/install.sh` - Added FastAPI and tool installation
- `scripts/update.sh` - Added tool rebuild and API update

---

## Installation & Update Commands

### Fresh Install
```bash
curl -sSL "https://raw.githubusercontent.com/caritechsolutions/Caricoder2/claude/video-transcoder-gstreamer-YnBIH/scripts/install.sh?$(date +%s)" | sudo bash
```

### Update Existing Installation
```bash
curl -sSL "https://raw.githubusercontent.com/caritechsolutions/Caricoder2/claude/video-transcoder-gstreamer-YnBIH/scripts/update.sh?$(date +%s)" | sudo bash -s -- -y
```

The `?$(date +%s)` ensures you always get the latest version without caching.

### Verify Installation
```bash
# Check services
systemctl status cari-api nginx php7.4-fpm

# Check tools
which player_preview udp_input

# Test API
curl http://localhost:8000/health

# Access web interface
echo "Web UI: http://$(hostname -I | awk '{print $1}'):8080"
```

---

## Known Issues

1. **Older libmicrohttpd**: Systems with libmicrohttpd < 0.9.71 may show compilation warnings (harmless)
2. **MP2 Audio**: Some browsers may not support MP2 audio playback natively
3. **Preview Startup**: First preview request may take 5-10 seconds while segments are generated

## Roadmap

- [ ] SRT input/output support in web UI
- [ ] Hardware transcoding profiles (NVENC, QuickSync)
- [ ] Multi-user support with role-based access
- [ ] Recording and time-shift functionality
- [ ] Clustering and failover support
