# A/V Sync Monitor Service

A monitoring service that measures audio/video synchronization quality for live MPEG-TS streams.

## Workflow

The program follows a 6-step workflow:

1. **CAPTURE** - Run `tsp` to extract PTS values from live stream (10 second sample)
2. **PARSE & SORT** - Extract PTS entries for video/audio PIDs, sort by PTS (presentation order)
3. **FILTER** - Keep only alternating video/audio frames (remove consecutive same-PID)
4. **COMPARE** - Calculate gaps between frame pairs, separate into A→V and V→A buckets
5. **ANALYZE** - Compare to baseline, detect patterns, assign status and score
6. **SAVE** - Store results for trending and API consumption

## Installation

```bash
# Install dependencies
sudo apt-get install libmicrohttpd-dev

# Build
make
sudo make install
```

## API Endpoints (Port 8082)

| Endpoint | Description |
|----------|-------------|
| `GET /health` | Health check |
| `GET /status` | All inputs current status |
| `GET /status/{id}` | Single input status |
| `GET /history/{id}` | Single input with 24-hour history |
| `GET /baseline` | All baselines |
| `GET /baseline/{id}` | Single input baseline |

## JSON Response Format

```json
{
  "id": "input1",
  "name": "Main Feed",
  "running": true,
  "current": {
    "timestamp": "2024-12-20 14:30:00",
    "audio_to_video": {
      "avg_ms": 210.5,
      "min_ms": 200.0,
      "max_ms": 220.0,
      "stddev_ms": 5.2,
      "count": 25
    },
    "video_to_audio": {
      "avg_ms": 45.0,
      "min_ms": 40.0,
      "max_ms": 50.0,
      "stddev_ms": 2.8,
      "count": 25
    },
    "variance_pct": 8.2,
    "pattern": "healthy",
    "status": "OK",
    "sync_score": 98,
    "anomalies": {
      "count": 0,
      "description": ""
    }
  },
  "baseline": {
    "a2v_avg_ms": 210.0,
    "v2a_avg_ms": 45.0,
    "sample_count": 100,
    "last_updated": "2024-12-20 14:30:00"
  },
  "trend": {
    "direction": "stable",
    "sample_count": 10
  }
}
```

## Status Thresholds

| Status | Condition |
|--------|-----------|
| **OK** | Deviation < 50ms from baseline |
| **WARNING** | Deviation 50-100ms from baseline, or anomalies detected |
| **CRITICAL** | Deviation > 100ms, or drifting pattern |

## Pattern Detection

| Pattern | Description |
|---------|-------------|
| **healthy** | Within ±10% of baseline, gaps consistent |
| **drifting** | Gaps steadily increasing or decreasing over time |
| **oscillating** | Gaps bouncing but returning to baseline (normal jitter) |
| **anomaly** | Sudden gap spikes > 100ms from mean |

## Sync Score (0-100)

The sync score is calculated based on:
- Variance from baseline (up to -40 points)
- Standard deviation of gaps (up to -20 points)
- Pattern issues (drifting: -20, anomaly: -15, oscillating: -5)
- Anomaly count (up to -20 points)

## Configuration

The service auto-discovers inputs from `/etc/caritrans/inputs/*.conf` files.

Required config fields:
```ini
[general]
name = input1
type = udp

[pids]
video = 211
audio = 221

[output]
address = 239.100.0.1
port = 10000
```

## Data Storage

- **Current data**: Saved to `/var/lib/caritrans/avsync.json`
- **History retention**: 288 samples (24 hours at 5-minute intervals)
- **Baseline**: Exponential moving average (alpha=0.1 for slow adaptation)

## Alerts Logic

| Condition | Result |
|-----------|--------|
| Current avg > baseline + 50ms | WARNING |
| Current avg > baseline + 100ms | CRITICAL |
| Gaps drifting (not oscillating) | CRITICAL |
| Single gap spike > expected range | Noted in anomalies, monitor next run |
