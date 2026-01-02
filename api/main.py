#!/usr/bin/env python3
"""
CariTranscoder Privileged API Service

This FastAPI service runs with root privileges to handle operations that
require elevated permissions, such as:
- Creating/modifying systemd service files
- Starting/stopping/restarting services
- Managing system configurations

The web interface (PHP) calls this API instead of using sudo.
"""

from fastapi import FastAPI, HTTPException, Body, Request
from fastapi.responses import JSONResponse
from pydantic import BaseModel, Field
import subprocess
import os
import json
import logging
from logging.handlers import RotatingFileHandler
from typing import Optional, Dict, Any, List
from datetime import datetime
import traceback
from starlette.middleware.base import BaseHTTPMiddleware
from starlette.responses import Response

# Configuration
CONFIG_DIR = "/etc/caritrans"
SYSTEMD_DIR = "/etc/systemd/system"
LOG_DIR = "/var/log/caritrans"
RUN_DIR = "/run/caritrans"
API_PORT = 8081

# Logging Configuration with rotation
log_formatter = logging.Formatter('%(asctime)s - %(name)s - %(levelname)s - %(message)s')

# Ensure log directory exists
os.makedirs(LOG_DIR, exist_ok=True)

# File handler with rotation - max 10MB, keep 3 backups
file_handler = RotatingFileHandler(
    f"{LOG_DIR}/cari-api.log",
    maxBytes=10*1024*1024,  # 10MB
    backupCount=3
)
file_handler.setFormatter(log_formatter)
file_handler.setLevel(logging.INFO)

# Console handler - only warnings and above
console_handler = logging.StreamHandler()
console_handler.setFormatter(log_formatter)
console_handler.setLevel(logging.WARNING)

# Configure root logger
logging.basicConfig(level=logging.INFO, handlers=[file_handler, console_handler])
logger = logging.getLogger(__name__)

app = FastAPI(
    title="CariTranscoder API",
    description="Privileged API for managing CariTranscoder services",
    version="1.0.0"
)


# Custom CORS middleware that handles credentials properly
class CORSMiddlewareCustom(BaseHTTPMiddleware):
    async def dispatch(self, request: Request, call_next):
        # Get the origin from the request
        origin = request.headers.get("origin", "")

        # Handle preflight requests
        if request.method == "OPTIONS":
            response = Response(status_code=200)
            response.headers["Access-Control-Allow-Origin"] = origin if origin else "*"
            response.headers["Access-Control-Allow-Credentials"] = "true"
            response.headers["Access-Control-Allow-Methods"] = "GET, POST, PUT, DELETE, OPTIONS"
            response.headers["Access-Control-Allow-Headers"] = "Content-Type, Authorization, Cookie, Cache-Control, Pragma"
            response.headers["Access-Control-Max-Age"] = "600"
            return response

        # Process the request
        response = await call_next(request)

        # Add CORS headers to response
        response.headers["Access-Control-Allow-Origin"] = origin if origin else "*"
        response.headers["Access-Control-Allow-Credentials"] = "true"
        response.headers["Access-Control-Allow-Methods"] = "GET, POST, PUT, DELETE, OPTIONS"
        response.headers["Access-Control-Allow-Headers"] = "Content-Type, Authorization, Cookie, Cache-Control, Pragma"

        return response

app.add_middleware(CORSMiddlewareCustom)


# ============================================================================
# Pydantic Models
# ============================================================================

class ServiceAction(BaseModel):
    """Model for service control actions"""
    action: str  # start, stop, restart, status
    service_name: str


class UDPInputService(BaseModel):
    """Model for creating UDP input service"""
    id: str
    source_address: str
    source_port: int
    output_address: str
    output_port: int
    api_port: int
    program: Optional[int] = None
    pids: Optional[str] = None  # Comma-separated list of PIDs to monitor (video,audio)
    description: Optional[str] = None


class SRTInputService(BaseModel):
    """Model for creating SRT input service"""
    id: str
    source_address: str
    source_port: int
    output_address: str
    output_port: int
    api_port: int
    mode: str = "caller"  # caller, listener, rendezvous
    latency: int = 120
    streamid: Optional[str] = None
    passphrase: Optional[str] = None
    pbkeylen: int = 0  # 0, 16, 24, 32
    program: Optional[int] = None
    pids: Optional[str] = None
    description: Optional[str] = None


class HLSInputService(BaseModel):
    """Model for creating HLS input service"""
    id: str
    source_url: str  # HLS manifest URL
    output_address: str
    output_port: int
    api_port: int
    live_mode: bool = True
    bitrate_mode: str = "auto"  # auto, highest, lowest, max, min
    bitrate_value: int = 0  # For max/min modes (kbps)
    highest_resolution: bool = False
    lowest_resolution: bool = False
    program: Optional[int] = None
    pids: Optional[str] = None
    description: Optional[str] = None


class HTTPInputService(BaseModel):
    """Model for creating HTTP input service"""
    id: str
    source_url: str  # HTTP URL for MPEG-TS stream
    output_address: str
    output_port: int
    api_port: int
    program: Optional[int] = None
    pids: Optional[str] = None
    description: Optional[str] = None


class RISTInputService(BaseModel):
    """Model for creating RIST input service"""
    id: str
    source_url: str  # RIST URL (e.g., rist://host:port)
    output_address: str
    output_port: int
    api_port: int
    buffer_size: int = 0  # Buffer size for retransmissions (ms), 0 = use ristreceiver default
    secret: Optional[str] = None  # Encryption secret
    encryption_type: int = 0  # 0=disabled, 128=AES-128, 256=AES-256
    profile: int = 1  # 0=simple, 1=main, 2=advanced
    program: Optional[int] = None
    pids: Optional[str] = None
    description: Optional[str] = None


class ABRVariant(BaseModel):
    """Model for ABR variant (video quality level)"""
    width: int = 1920
    height: int = 1080
    bitrate: int = 5000000  # bits/sec
    video_pid: int = 100


class TranscoderService(BaseModel):
    """Model for creating transcoder service with tsp CBR output"""
    id: str
    name: str
    input_address: str
    input_port: int
    output_address: str
    output_port: int
    api_port: int = 9200
    video_pid: int = 256  # Video elementary stream PID (default 0x100)
    audio_pid: int = 257  # Audio elementary stream PID (default 0x101)
    program_number: int = 1  # MPEG-TS program number
    tsp_bitrate: int  # CBR bitrate for tsp output (video+audio+5%)

    # Video settings
    video_mode: str = "transcode"  # transcode, passthrough, drop
    video_codec: str = "h264"  # h264, h265, mpeg2
    video_bitrate: int = 5000000
    video_preset: str = "superfast"
    keyframe_interval: int = 60
    profile: str = "main"  # baseline, main, high
    bframes: int = 0
    ref: int = 1
    qp_min: int = 10
    qp_max: int = 51
    vbv_bufsize: int = 600
    video_threads: int = 0
    sliced_threads: bool = True
    cabac: bool = True
    trellis: bool = False
    aud: bool = True
    intra_refresh: bool = False
    interlaced: bool = False
    psy_tune: str = ""
    x264_opts: str = ""

    # Scaling settings
    scaling_enabled: bool = False
    scale_width: int = 1920
    scale_height: int = 1080
    scale_method: int = 1
    add_borders: bool = False
    scale_threads: int = 0
    deinterlace: bool = False

    # Audio settings
    audio_mode: str = "transcode"  # transcode, passthrough, drop
    audio_codec: str = "aac"  # aac, ac3, mp2
    audio_bitrate: int = 128000
    audio_channels: int = 2
    audio_samplerate: int = 48000

    # ABR settings
    abr_enabled: bool = False
    variants: Optional[List[ABRVariant]] = None

    description: Optional[str] = None


class ServiceFile(BaseModel):
    """Model for generic service file creation"""
    service_name: str
    content: str


class ConfigFile(BaseModel):
    """Model for config file operations"""
    path: str
    content: str


# ============================================================================
# Helper Functions
# ============================================================================

def run_systemctl(action: str, service: str) -> Dict[str, Any]:
    """Run a systemctl command and return the result"""
    try:
        if action == "daemon-reload":
            cmd = ["systemctl", "daemon-reload"]
        else:
            cmd = ["systemctl", action, service]

        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=30
        )
        return {
            "success": result.returncode == 0,
            "stdout": result.stdout,
            "stderr": result.stderr,
            "returncode": result.returncode
        }
    except subprocess.TimeoutExpired:
        return {
            "success": False,
            "error": "Command timed out"
        }
    except Exception as e:
        return {
            "success": False,
            "error": str(e)
        }


def daemon_reload() -> Dict[str, Any]:
    """Reload systemd daemon"""
    return run_systemctl("daemon-reload", "")


def get_service_status(service: str) -> Dict[str, Any]:
    """Get detailed status of a service"""
    result = run_systemctl("is-active", service)
    is_active = result.get("stdout", "").strip() == "active"

    # Get more detailed status
    try:
        status_result = subprocess.run(
            ["systemctl", "show", service, "--property=ActiveState,SubState,MainPID,LoadState"],
            capture_output=True,
            text=True
        )

        props = {}
        for line in status_result.stdout.strip().split('\n'):
            if '=' in line:
                key, value = line.split('=', 1)
                props[key] = value

        return {
            "service": service,
            "is_active": is_active,
            "active_state": props.get("ActiveState", "unknown"),
            "sub_state": props.get("SubState", "unknown"),
            "main_pid": props.get("MainPID", "0"),
            "load_state": props.get("LoadState", "unknown")
        }
    except Exception as e:
        logger.error(f"Error getting service status: {e}")
        return {
            "service": service,
            "is_active": is_active,
            "active_state": "unknown",
            "sub_state": "unknown",
            "main_pid": "0",
            "load_state": "unknown"
        }


def generate_udp_input_service_file(service_data: UDPInputService) -> str:
    """Generate systemd service file content for UDP input"""

    # Unique log file for this input
    log_file = f"/var/log/caritrans/udp-input-{service_data.id}.log"

    # Build the command
    cmd_parts = [
        "/usr/local/bin/udp_input",
        f"--input {service_data.source_address}:{service_data.source_port}",
        f"--output {service_data.output_address}:{service_data.output_port}",
        f"--api-port {service_data.api_port}",
        f"--log-file {log_file}"
    ]

    if service_data.program is not None:
        cmd_parts.append(f"--program {service_data.program}")

    if service_data.pids:
        cmd_parts.append(f"--pids {service_data.pids}")

    exec_start = " ".join(cmd_parts)
    description = service_data.description or f"CariTranscoder UDP Input - {service_data.id}"

    service_content = f"""[Unit]
Description={description}
Documentation=https://github.com/caritechsolutions/caritranscoder
After=network.target
Wants=network-online.target

[Service]
Type=simple
User=root
Group=root

# Main process
ExecStart={exec_start}
ExecReload=/bin/kill -HUP $MAINPID

# Restart behavior
Restart=always
RestartSec=5
StartLimitIntervalSec=60
StartLimitBurst=5

# Resource limits
LimitNOFILE=65535
LimitNPROC=4096

# Logging
StandardOutput=journal
StandardError=journal
SyslogIdentifier=cari-udp-{service_data.id}

[Install]
WantedBy=multi-user.target
"""
    return service_content


def generate_srt_input_service_file(service_data: SRTInputService) -> str:
    """Generate systemd service file content for SRT input"""

    # Unique log file for this input
    log_file = f"/var/log/caritrans/srt-input-{service_data.id}.log"

    # Build the command
    cmd_parts = [
        "/usr/local/bin/srt_input",
        f"--id {service_data.id}",
        f"--address {service_data.source_address}",
        f"--port {service_data.source_port}",
        f"--mode {service_data.mode}",
        f"--latency {service_data.latency}",
        f"--output {service_data.output_address}:{service_data.output_port}",
        f"--api-port {service_data.api_port}",
        f"--log-file {log_file}"
    ]

    if service_data.streamid:
        cmd_parts.append(f"--streamid '{service_data.streamid}'")

    if service_data.passphrase:
        cmd_parts.append(f"--passphrase '{service_data.passphrase}'")
        if service_data.pbkeylen > 0:
            cmd_parts.append(f"--pbkeylen {service_data.pbkeylen}")

    if service_data.program is not None:
        cmd_parts.append(f"--program {service_data.program}")

    if service_data.pids:
        cmd_parts.append(f"--pids {service_data.pids}")

    exec_start = " ".join(cmd_parts)
    description = service_data.description or f"CariTranscoder SRT Input - {service_data.id}"

    service_content = f"""[Unit]
Description={description}
Documentation=https://github.com/caritechsolutions/caritranscoder
After=network.target
Wants=network-online.target
StartLimitIntervalSec=60
StartLimitBurst=5

[Service]
Type=simple
User=root
Group=root

# Main process
ExecStart={exec_start}
ExecReload=/bin/kill -HUP $MAINPID

# Restart behavior
Restart=always
RestartSec=5

# Resource limits
LimitNOFILE=65535
LimitNPROC=4096

# Logging
StandardOutput=journal
StandardError=journal
SyslogIdentifier=cari-srt-{service_data.id}

[Install]
WantedBy=multi-user.target
"""
    return service_content


def generate_hls_input_service_file(service_data: HLSInputService) -> str:
    """Generate systemd service file content for HLS input"""

    # Unique log file for this input
    log_file = f"/var/log/caritrans/hls-input-{service_data.id}.log"

    # Build the command
    cmd_parts = [
        "/usr/local/bin/hls_input",
        f"--url '{service_data.source_url}'",
        f"--output {service_data.output_address}:{service_data.output_port}",
        f"--api-port {service_data.api_port}",
        f"--log-file {log_file}"
    ]

    if service_data.live_mode:
        cmd_parts.append("--live")

    # Bitrate selection
    if service_data.bitrate_mode == "highest":
        cmd_parts.append("--highest-bitrate")
    elif service_data.bitrate_mode == "lowest":
        cmd_parts.append("--lowest-bitrate")
    elif service_data.bitrate_mode == "max" and service_data.bitrate_value > 0:
        cmd_parts.append(f"--max-bitrate {service_data.bitrate_value}")
    elif service_data.bitrate_mode == "min" and service_data.bitrate_value > 0:
        cmd_parts.append(f"--min-bitrate {service_data.bitrate_value}")

    # Resolution selection
    if service_data.highest_resolution:
        cmd_parts.append("--highest-resolution")
    elif service_data.lowest_resolution:
        cmd_parts.append("--lowest-resolution")

    if service_data.program is not None:
        cmd_parts.append(f"--program {service_data.program}")

    if service_data.pids:
        cmd_parts.append(f"--pids {service_data.pids}")

    exec_start = " ".join(cmd_parts)
    description = service_data.description or f"CariTranscoder HLS Input - {service_data.id}"

    service_content = f"""[Unit]
Description={description}
Documentation=https://github.com/caritechsolutions/caritranscoder
After=network.target
Wants=network-online.target
StartLimitIntervalSec=60
StartLimitBurst=5

[Service]
Type=simple
User=root
Group=root

# Main process
ExecStart={exec_start}
ExecReload=/bin/kill -HUP $MAINPID

# Restart behavior
Restart=always
RestartSec=5

# Resource limits
LimitNOFILE=65535
LimitNPROC=4096

# Logging
StandardOutput=journal
StandardError=journal
SyslogIdentifier=cari-hls-{service_data.id}

[Install]
WantedBy=multi-user.target
"""
    return service_content


def generate_http_input_service_file(service_data: HTTPInputService) -> str:
    """Generate systemd service file content for HTTP input"""

    # Unique log file for this input
    log_file = f"/var/log/caritrans/http-input-{service_data.id}.log"

    # Build the command
    cmd_parts = [
        "/usr/local/bin/http_input",
        f"--url '{service_data.source_url}'",
        f"--output {service_data.output_address}:{service_data.output_port}",
        f"--api-port {service_data.api_port}",
        f"--log-file {log_file}"
    ]

    if service_data.program is not None:
        cmd_parts.append(f"--program {service_data.program}")

    if service_data.pids:
        cmd_parts.append(f"--pids {service_data.pids}")

    exec_start = " ".join(cmd_parts)
    description = service_data.description or f"CariTranscoder HTTP Input - {service_data.id}"

    service_content = f"""[Unit]
Description={description}
Documentation=https://github.com/caritechsolutions/caritranscoder
After=network.target
Wants=network-online.target
StartLimitIntervalSec=60
StartLimitBurst=5

[Service]
Type=simple
User=root
Group=root

# Main process
ExecStart={exec_start}
ExecReload=/bin/kill -HUP $MAINPID

# Restart behavior
Restart=always
RestartSec=5

# Resource limits
LimitNOFILE=65535
LimitNPROC=4096

# Logging
StandardOutput=journal
StandardError=journal
SyslogIdentifier=cari-http-{service_data.id}

[Install]
WantedBy=multi-user.target
"""
    return service_content


def generate_rist_input_service_file(service_data: RISTInputService) -> str:
    """Generate systemd service file content for RIST input"""

    # Unique log file for this input
    log_file = f"/var/log/caritrans/rist-input-{service_data.id}.log"

    # Build the command
    cmd_parts = [
        "/usr/local/bin/rist_input",
        f"--url '{service_data.source_url}'",
        f"--output {service_data.output_address}:{service_data.output_port}",
        f"--api-port {service_data.api_port}",
        f"--log-file {log_file}"
    ]

    # Only add buffer if explicitly set (> 0)
    if service_data.buffer_size > 0:
        cmd_parts.append(f"--buffer {service_data.buffer_size}")

    if service_data.secret:
        cmd_parts.append(f"--secret '{service_data.secret}'")
        cmd_parts.append(f"--encryption {service_data.encryption_type}")

    if service_data.program is not None:
        cmd_parts.append(f"--program {service_data.program}")

    if service_data.pids:
        cmd_parts.append(f"--pids {service_data.pids}")

    exec_start = " ".join(cmd_parts)
    description = service_data.description or f"CariTranscoder RIST Input - {service_data.id}"

    service_content = f"""[Unit]
Description={description}
Documentation=https://github.com/caritechsolutions/caritranscoder
After=network.target
Wants=network-online.target
StartLimitIntervalSec=60
StartLimitBurst=5

[Service]
Type=simple
User=root
Group=root

# Main process
ExecStart={exec_start}
ExecReload=/bin/kill -HUP $MAINPID

# Restart behavior
Restart=always
RestartSec=5

# Resource limits
LimitNOFILE=65535
LimitNPROC=4096

# Logging
StandardOutput=journal
StandardError=journal
SyslogIdentifier=cari-rist-{service_data.id}

[Install]
WantedBy=multi-user.target
"""
    return service_content


# ============================================================================
# API Endpoints
# ============================================================================

@app.get("/")
async def root():
    """API health check"""
    return {
        "status": "ok",
        "service": "CariTranscoder API",
        "version": "1.0.0"
    }


@app.get("/health")
async def health():
    """Health check endpoint"""
    return {
        "status": "healthy",
        "timestamp": datetime.now().isoformat()
    }


# ----------------------------------------------------------------------------
# Service Management
# ----------------------------------------------------------------------------

@app.post("/service/control")
async def control_service(action: ServiceAction):
    """Control a systemd service (start, stop, restart, status)"""
    valid_actions = ["start", "stop", "restart", "status", "enable", "disable"]

    if action.action not in valid_actions:
        raise HTTPException(status_code=400, detail=f"Invalid action. Must be one of: {valid_actions}")

    # Security: only allow cari-* services
    if not action.service_name.startswith("cari-"):
        raise HTTPException(status_code=403, detail="Can only control cari-* services")

    logger.info(f"Service control: {action.action} {action.service_name}")

    if action.action == "status":
        return get_service_status(action.service_name)

    result = run_systemctl(action.action, action.service_name)
    return {
        "action": action.action,
        "service": action.service_name,
        **result
    }


@app.get("/service/status/{service_name}")
async def service_status(service_name: str):
    """Get status of a specific service"""
    if not service_name.startswith("cari-"):
        raise HTTPException(status_code=403, detail="Can only query cari-* services")

    return get_service_status(service_name)


@app.get("/services/list")
async def list_services():
    """List all cari-* services and their status"""
    try:
        result = subprocess.run(
            ["systemctl", "list-units", "--type=service", "--all", "--no-pager", "--plain"],
            capture_output=True,
            text=True
        )

        services = []
        for line in result.stdout.split('\n'):
            if 'cari-' in line:
                parts = line.split()
                if len(parts) >= 4:
                    services.append({
                        "name": parts[0],
                        "load": parts[1],
                        "active": parts[2],
                        "sub": parts[3]
                    })

        return {"services": services}
    except Exception as e:
        logger.error(f"Error listing services: {e}")
        raise HTTPException(status_code=500, detail=str(e))


# ----------------------------------------------------------------------------
# UDP Input Service Management
# ----------------------------------------------------------------------------

@app.post("/input/udp/create")
async def create_udp_input_service(service: UDPInputService):
    """Create a systemd service file for UDP input"""

    service_name = f"cari-udp-{service.id}"
    service_file = f"{SYSTEMD_DIR}/{service_name}.service"

    try:
        # Generate service content
        service_content = generate_udp_input_service_file(service)

        # Write the service file directly (we're running as root)
        with open(service_file, 'w') as f:
            f.write(service_content)

        logger.info(f"Created service file: {service_file}")

        # Reload systemd
        daemon_reload()

        return {
            "success": True,
            "service_name": service_name,
            "service_file": service_file,
            "message": f"Service file created: {service_file}"
        }
    except Exception as e:
        logger.error(f"Failed to create service file: {e}")
        logger.error(traceback.format_exc())
        raise HTTPException(status_code=500, detail=str(e))


@app.delete("/input/udp/{input_id}")
async def delete_udp_input_service(input_id: str):
    """Delete a UDP input service"""

    service_name = f"cari-udp-{input_id}.service"
    service_file = f"{SYSTEMD_DIR}/{service_name}"

    try:
        # Stop the service first
        run_systemctl("stop", service_name)
        run_systemctl("disable", service_name)

        # Remove the service file
        if os.path.exists(service_file):
            os.remove(service_file)
            logger.info(f"Deleted service file: {service_file}")

        # Reload systemd
        daemon_reload()

        return {
            "success": True,
            "message": f"Service {service_name} deleted"
        }
    except Exception as e:
        logger.error(f"Failed to delete service: {e}")
        raise HTTPException(status_code=500, detail=str(e))


@app.post("/input/udp/{input_id}/start")
async def start_udp_input(input_id: str):
    """Start a UDP input service"""
    service_name = f"cari-udp-{input_id}.service"

    # Check if service file exists
    service_file = f"{SYSTEMD_DIR}/{service_name}"
    if not os.path.exists(service_file):
        logger.error(f"Service file not found: {service_file}")
        return {
            "success": False,
            "service": service_name,
            "error": f"Service file not found: {service_file}"
        }

    # Enable and start
    enable_result = run_systemctl("enable", service_name)
    if not enable_result.get("success", False):
        logger.error(f"Failed to enable service: {enable_result}")

    result = run_systemctl("start", service_name)
    logger.info(f"Started service: {service_name}, result: {result}")

    return {
        "success": result.get("success", False),
        "service": service_name,
        **result
    }


@app.post("/input/udp/{input_id}/stop")
async def stop_udp_input(input_id: str):
    """Stop a UDP input service"""
    service_name = f"cari-udp-{input_id}.service"

    result = run_systemctl("stop", service_name)
    logger.info(f"Stopped service: {service_name}, result: {result}")

    return {
        "success": result.get("success", False),
        "service": service_name,
        **result
    }


@app.post("/input/udp/{input_id}/restart")
async def restart_udp_input(input_id: str):
    """Restart a UDP input service"""
    service_name = f"cari-udp-{input_id}.service"

    result = run_systemctl("restart", service_name)
    logger.info(f"Restarted service: {service_name}, result: {result}")

    return {
        "success": result.get("success", False),
        "service": service_name,
        **result
    }


@app.get("/input/udp/{input_id}/status")
async def udp_input_status(input_id: str):
    """Get status of a UDP input service"""
    service_name = f"cari-udp-{input_id}.service"
    return get_service_status(service_name)


# ----------------------------------------------------------------------------
# SRT Input Service Management
# ----------------------------------------------------------------------------

@app.post("/input/srt/create")
async def create_srt_input_service(service: SRTInputService):
    """Create a systemd service file for SRT input"""

    service_name = f"cari-srt-{service.id}"
    service_file = f"{SYSTEMD_DIR}/{service_name}.service"

    try:
        # Generate service content
        service_content = generate_srt_input_service_file(service)

        # Write the service file directly (we're running as root)
        with open(service_file, 'w') as f:
            f.write(service_content)

        logger.info(f"Created SRT service file: {service_file}")

        # Reload systemd
        daemon_reload()

        return {
            "success": True,
            "service_name": service_name,
            "service_file": service_file,
            "message": f"Service file created: {service_file}"
        }
    except Exception as e:
        logger.error(f"Failed to create SRT service file: {e}")
        logger.error(traceback.format_exc())
        raise HTTPException(status_code=500, detail=str(e))


@app.delete("/input/srt/{input_id}")
async def delete_srt_input_service(input_id: str):
    """Delete a SRT input service"""

    service_name = f"cari-srt-{input_id}.service"
    service_file = f"{SYSTEMD_DIR}/{service_name}"

    try:
        # Stop the service first
        run_systemctl("stop", service_name)
        run_systemctl("disable", service_name)

        # Remove the service file
        if os.path.exists(service_file):
            os.remove(service_file)
            logger.info(f"Deleted SRT service file: {service_file}")

        # Reload systemd
        daemon_reload()

        return {
            "success": True,
            "message": f"Service {service_name} deleted"
        }
    except Exception as e:
        logger.error(f"Failed to delete SRT service: {e}")
        raise HTTPException(status_code=500, detail=str(e))


@app.post("/input/srt/{input_id}/start")
async def start_srt_input(input_id: str):
    """Start a SRT input service"""
    service_name = f"cari-srt-{input_id}.service"

    # Check if service file exists
    service_file = f"{SYSTEMD_DIR}/{service_name}"
    if not os.path.exists(service_file):
        logger.error(f"SRT service file not found: {service_file}")
        return {
            "success": False,
            "service": service_name,
            "error": f"Service file not found: {service_file}"
        }

    # Enable and start
    enable_result = run_systemctl("enable", service_name)
    if not enable_result.get("success", False):
        logger.error(f"Failed to enable SRT service: {enable_result}")

    result = run_systemctl("start", service_name)
    logger.info(f"Started SRT service: {service_name}, result: {result}")

    return {
        "success": result.get("success", False),
        "service": service_name,
        **result
    }


@app.post("/input/srt/{input_id}/stop")
async def stop_srt_input(input_id: str):
    """Stop a SRT input service"""
    service_name = f"cari-srt-{input_id}.service"

    result = run_systemctl("stop", service_name)
    logger.info(f"Stopped SRT service: {service_name}, result: {result}")

    return {
        "success": result.get("success", False),
        "service": service_name,
        **result
    }


@app.post("/input/srt/{input_id}/restart")
async def restart_srt_input(input_id: str):
    """Restart a SRT input service"""
    service_name = f"cari-srt-{input_id}.service"

    result = run_systemctl("restart", service_name)
    logger.info(f"Restarted SRT service: {service_name}, result: {result}")

    return {
        "success": result.get("success", False),
        "service": service_name,
        **result
    }


@app.get("/input/srt/{input_id}/status")
async def srt_input_status(input_id: str):
    """Get status of a SRT input service"""
    service_name = f"cari-srt-{input_id}.service"
    return get_service_status(service_name)


# ----------------------------------------------------------------------------
# HLS Input Service Management
# ----------------------------------------------------------------------------

@app.post("/input/hls/create")
async def create_hls_input_service(service: HLSInputService):
    """Create a systemd service file for HLS input"""

    service_name = f"cari-hls-{service.id}"
    service_file = f"{SYSTEMD_DIR}/{service_name}.service"

    try:
        # Generate service content
        service_content = generate_hls_input_service_file(service)

        # Write the service file
        with open(service_file, 'w') as f:
            f.write(service_content)

        # Reload systemd
        daemon_reload()

        logger.info(f"Created HLS input service: {service_name}")

        return {
            "success": True,
            "service_name": service_name,
            "service_file": service_file,
            "message": f"HLS input service '{service_name}' created successfully"
        }
    except Exception as e:
        logger.error(f"Failed to create HLS service: {e}")
        raise HTTPException(status_code=500, detail=str(e))


@app.delete("/input/hls/{input_id}")
async def delete_hls_input_service(input_id: str):
    """Delete a HLS input service"""

    service_name = f"cari-hls-{input_id}.service"
    service_file = f"{SYSTEMD_DIR}/{service_name}"

    try:
        # Stop the service first
        run_systemctl("stop", service_name)
        run_systemctl("disable", service_name)

        # Remove the service file
        if os.path.exists(service_file):
            os.remove(service_file)

        # Reload systemd
        daemon_reload()

        logger.info(f"Deleted HLS input service: {service_name}")

        return {
            "success": True,
            "service_name": service_name,
            "message": f"HLS input service '{service_name}' deleted successfully"
        }
    except Exception as e:
        logger.error(f"Failed to delete HLS service: {e}")
        raise HTTPException(status_code=500, detail=str(e))


@app.post("/input/hls/{input_id}/start")
async def start_hls_input(input_id: str):
    """Start a HLS input service"""
    service_name = f"cari-hls-{input_id}.service"

    # Check if service file exists
    service_file = f"{SYSTEMD_DIR}/{service_name}"
    if not os.path.exists(service_file):
        logger.error(f"HLS service file not found: {service_file}")
        return {
            "success": False,
            "error": f"Service file not found: {service_file}"
        }

    # Enable and start the service
    run_systemctl("enable", service_name)
    result = run_systemctl("start", service_name)
    logger.info(f"Started HLS service: {service_name}, result: {result}")

    return {
        "success": result.get("success", False),
        "service": service_name,
        "stdout": result.get("stdout", ""),
        "stderr": result.get("stderr", "")
    }


@app.post("/input/hls/{input_id}/stop")
async def stop_hls_input(input_id: str):
    """Stop a HLS input service"""
    service_name = f"cari-hls-{input_id}.service"

    result = run_systemctl("stop", service_name)
    logger.info(f"Stopped HLS service: {service_name}, result: {result}")

    return {
        "success": result.get("success", False),
        "service": service_name,
        "stdout": result.get("stdout", ""),
        "stderr": result.get("stderr", "")
    }


@app.post("/input/hls/{input_id}/restart")
async def restart_hls_input(input_id: str):
    """Restart a HLS input service"""
    service_name = f"cari-hls-{input_id}.service"

    result = run_systemctl("restart", service_name)
    logger.info(f"Restarted HLS service: {service_name}, result: {result}")

    return {
        "success": result.get("success", False),
        "service": service_name,
        "stdout": result.get("stdout", ""),
        "stderr": result.get("stderr", "")
    }


@app.get("/input/hls/{input_id}/status")
async def hls_input_status(input_id: str):
    """Get status of a HLS input service"""
    service_name = f"cari-hls-{input_id}.service"
    return get_service_status(service_name)


# ----------------------------------------------------------------------------
# HTTP Input Service Management
# ----------------------------------------------------------------------------

@app.post("/input/http/create")
async def create_http_input_service(service: HTTPInputService):
    """Create a systemd service file for HTTP input"""

    service_name = f"cari-http-{service.id}"
    service_file = f"{SYSTEMD_DIR}/{service_name}.service"

    try:
        # Generate service content
        service_content = generate_http_input_service_file(service)

        # Write the service file
        with open(service_file, 'w') as f:
            f.write(service_content)

        # Reload systemd
        daemon_reload()

        logger.info(f"Created HTTP input service: {service_name}")

        return {
            "success": True,
            "service_name": service_name,
            "service_file": service_file,
            "message": f"HTTP input service '{service_name}' created successfully"
        }
    except Exception as e:
        logger.error(f"Failed to create HTTP service: {e}")
        raise HTTPException(status_code=500, detail=str(e))


@app.delete("/input/http/{input_id}")
async def delete_http_input_service(input_id: str):
    """Delete a HTTP input service"""

    service_name = f"cari-http-{input_id}.service"
    service_file = f"{SYSTEMD_DIR}/{service_name}"

    try:
        # Stop the service first
        run_systemctl("stop", service_name)
        run_systemctl("disable", service_name)

        # Remove the service file
        if os.path.exists(service_file):
            os.remove(service_file)

        # Reload systemd
        daemon_reload()

        logger.info(f"Deleted HTTP input service: {service_name}")

        return {
            "success": True,
            "service_name": service_name,
            "message": f"HTTP input service '{service_name}' deleted successfully"
        }
    except Exception as e:
        logger.error(f"Failed to delete HTTP service: {e}")
        raise HTTPException(status_code=500, detail=str(e))


@app.post("/input/http/{input_id}/start")
async def start_http_input(input_id: str):
    """Start a HTTP input service"""
    service_name = f"cari-http-{input_id}.service"

    # Check if service file exists
    service_file = f"{SYSTEMD_DIR}/{service_name}"
    if not os.path.exists(service_file):
        logger.error(f"HTTP service file not found: {service_file}")
        return {
            "success": False,
            "error": f"Service file not found: {service_file}"
        }

    # Enable and start the service
    run_systemctl("enable", service_name)
    result = run_systemctl("start", service_name)
    logger.info(f"Started HTTP service: {service_name}, result: {result}")

    return {
        "success": result.get("success", False),
        "service": service_name,
        "stdout": result.get("stdout", ""),
        "stderr": result.get("stderr", "")
    }


@app.post("/input/http/{input_id}/stop")
async def stop_http_input(input_id: str):
    """Stop a HTTP input service"""
    service_name = f"cari-http-{input_id}.service"

    result = run_systemctl("stop", service_name)
    logger.info(f"Stopped HTTP service: {service_name}, result: {result}")

    return {
        "success": result.get("success", False),
        "service": service_name,
        "stdout": result.get("stdout", ""),
        "stderr": result.get("stderr", "")
    }


@app.post("/input/http/{input_id}/restart")
async def restart_http_input(input_id: str):
    """Restart a HTTP input service"""
    service_name = f"cari-http-{input_id}.service"

    result = run_systemctl("restart", service_name)
    logger.info(f"Restarted HTTP service: {service_name}, result: {result}")

    return {
        "success": result.get("success", False),
        "service": service_name,
        "stdout": result.get("stdout", ""),
        "stderr": result.get("stderr", "")
    }


@app.get("/input/http/{input_id}/status")
async def http_input_status(input_id: str):
    """Get status of a HTTP input service"""
    service_name = f"cari-http-{input_id}.service"
    return get_service_status(service_name)


# ----------------------------------------------------------------------------
# RIST Input Service Management
# ----------------------------------------------------------------------------

@app.post("/input/rist/create")
async def create_rist_input_service(service: RISTInputService):
    """Create a systemd service file for RIST input"""

    service_name = f"cari-rist-{service.id}"
    service_file = f"{SYSTEMD_DIR}/{service_name}.service"

    try:
        # Generate service content
        service_content = generate_rist_input_service_file(service)

        # Write the service file
        with open(service_file, 'w') as f:
            f.write(service_content)

        # Reload systemd
        daemon_reload()

        logger.info(f"Created RIST input service: {service_name}")

        return {
            "success": True,
            "service_name": service_name,
            "service_file": service_file,
            "message": f"RIST input service '{service_name}' created successfully"
        }
    except Exception as e:
        logger.error(f"Failed to create RIST service: {e}")
        raise HTTPException(status_code=500, detail=str(e))


@app.delete("/input/rist/{input_id}")
async def delete_rist_input_service(input_id: str):
    """Delete a RIST input service"""

    service_name = f"cari-rist-{input_id}.service"
    service_file = f"{SYSTEMD_DIR}/{service_name}"

    try:
        # Stop the service first
        run_systemctl("stop", service_name)
        run_systemctl("disable", service_name)

        # Remove the service file
        if os.path.exists(service_file):
            os.remove(service_file)

        # Reload systemd
        daemon_reload()

        logger.info(f"Deleted RIST input service: {service_name}")

        return {
            "success": True,
            "service_name": service_name,
            "message": f"RIST input service '{service_name}' deleted successfully"
        }
    except Exception as e:
        logger.error(f"Failed to delete RIST service: {e}")
        raise HTTPException(status_code=500, detail=str(e))


@app.post("/input/rist/{input_id}/start")
async def start_rist_input(input_id: str):
    """Start a RIST input service"""
    service_name = f"cari-rist-{input_id}.service"

    # Check if service file exists
    service_file = f"{SYSTEMD_DIR}/{service_name}"
    if not os.path.exists(service_file):
        logger.error(f"RIST service file not found: {service_file}")
        return {
            "success": False,
            "error": f"Service file not found: {service_file}"
        }

    # Enable and start the service
    run_systemctl("enable", service_name)
    result = run_systemctl("start", service_name)
    logger.info(f"Started RIST service: {service_name}, result: {result}")

    return {
        "success": result.get("success", False),
        "service": service_name,
        "stdout": result.get("stdout", ""),
        "stderr": result.get("stderr", "")
    }


@app.post("/input/rist/{input_id}/stop")
async def stop_rist_input(input_id: str):
    """Stop a RIST input service"""
    service_name = f"cari-rist-{input_id}.service"

    result = run_systemctl("stop", service_name)
    logger.info(f"Stopped RIST service: {service_name}, result: {result}")

    return {
        "success": result.get("success", False),
        "service": service_name,
        "stdout": result.get("stdout", ""),
        "stderr": result.get("stderr", "")
    }


@app.post("/input/rist/{input_id}/restart")
async def restart_rist_input(input_id: str):
    """Restart a RIST input service"""
    service_name = f"cari-rist-{input_id}.service"

    result = run_systemctl("restart", service_name)
    logger.info(f"Restarted RIST service: {service_name}, result: {result}")

    return {
        "success": result.get("success", False),
        "service": service_name,
        "stdout": result.get("stdout", ""),
        "stderr": result.get("stderr", "")
    }


@app.get("/input/rist/{input_id}/status")
async def rist_input_status(input_id: str):
    """Get status of a RIST input service"""
    service_name = f"cari-rist-{input_id}.service"
    return get_service_status(service_name)


# ----------------------------------------------------------------------------
# Generic Service File Management
# ----------------------------------------------------------------------------

@app.post("/service/file/create")
async def create_service_file(service: ServiceFile):
    """Create a generic systemd service file"""

    # Security: only allow cari-* services
    if not service.service_name.startswith("cari-"):
        raise HTTPException(status_code=403, detail="Can only create cari-* services")

    service_file = f"{SYSTEMD_DIR}/{service.service_name}.service"

    try:
        with open(service_file, 'w') as f:
            f.write(service.content)

        logger.info(f"Created service file: {service_file}")
        daemon_reload()

        return {
            "success": True,
            "service_file": service_file
        }
    except Exception as e:
        logger.error(f"Failed to create service file: {e}")
        raise HTTPException(status_code=500, detail=str(e))


@app.delete("/service/file/{service_name}")
async def delete_service_file(service_name: str):
    """Delete a systemd service file"""

    # Security: only allow cari-* services
    if not service_name.startswith("cari-"):
        raise HTTPException(status_code=403, detail="Can only delete cari-* services")

    service_file = f"{SYSTEMD_DIR}/{service_name}.service"

    try:
        # Stop and disable first
        run_systemctl("stop", service_name)
        run_systemctl("disable", service_name)

        if os.path.exists(service_file):
            os.remove(service_file)
            logger.info(f"Deleted service file: {service_file}")

        daemon_reload()

        return {
            "success": True,
            "message": f"Service file deleted: {service_file}"
        }
    except Exception as e:
        logger.error(f"Failed to delete service file: {e}")
        raise HTTPException(status_code=500, detail=str(e))


# ----------------------------------------------------------------------------
# Config File Management (for privileged config locations)
# ----------------------------------------------------------------------------

@app.post("/config/write")
async def write_config(config: ConfigFile):
    """Write a config file (for files requiring elevated permissions)"""
    from pathlib import Path

    # Security: only allow writing to caritrans directories
    allowed_paths = [CONFIG_DIR, LOG_DIR, RUN_DIR]
    path = Path(config.path)

    is_allowed = any(str(path).startswith(p) for p in allowed_paths)
    if not is_allowed:
        raise HTTPException(status_code=403, detail=f"Can only write to {allowed_paths}")

    try:
        # Create parent directory if needed
        path.parent.mkdir(parents=True, exist_ok=True)

        with open(path, 'w') as f:
            f.write(config.content)

        logger.info(f"Wrote config file: {path}")

        return {
            "success": True,
            "path": str(path)
        }
    except Exception as e:
        logger.error(f"Failed to write config: {e}")
        raise HTTPException(status_code=500, detail=str(e))


@app.delete("/config/delete")
async def delete_config(path: str):
    """Delete a config file"""
    from pathlib import Path

    # Security: only allow deleting from caritrans directories
    allowed_paths = [CONFIG_DIR, LOG_DIR, RUN_DIR]
    file_path = Path(path)

    is_allowed = any(str(file_path).startswith(p) for p in allowed_paths)
    if not is_allowed:
        raise HTTPException(status_code=403, detail=f"Can only delete from {allowed_paths}")

    try:
        if file_path.exists():
            file_path.unlink()
            logger.info(f"Deleted config file: {path}")

        return {
            "success": True,
            "message": f"Deleted: {path}"
        }
    except Exception as e:
        logger.error(f"Failed to delete config: {e}")
        raise HTTPException(status_code=500, detail=str(e))


# ----------------------------------------------------------------------------
# Preview Management (player_preview)
# ----------------------------------------------------------------------------

class PreviewStart(BaseModel):
    """Model for starting a preview"""
    input_address: str  # e.g., "239.100.0.1:10000"
    output_dir: str     # e.g., "/var/www/html/caritrans/preview/bet"
    api_port: int       # e.g., 10100
    folder: str         # e.g., "bet" (for log file naming)


def is_preview_running(api_port: int) -> bool:
    """Check if player_preview is running on the given port"""
    import socket
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(1)
    try:
        result = sock.connect_ex(('127.0.0.1', api_port))
        return result == 0
    except:
        return False
    finally:
        sock.close()


@app.post("/preview/start")
async def start_preview(preview: PreviewStart):
    """Start player_preview for an input"""

    # Check if already running
    if is_preview_running(preview.api_port):
        return {
            "success": True,
            "already_running": True,
            "message": "Preview already running",
            "api_port": preview.api_port
        }

    # Create output directory if needed
    os.makedirs(preview.output_dir, exist_ok=True)

    # Check binary exists
    binary = "/usr/local/bin/player_preview"
    if not os.path.exists(binary):
        return {
            "success": False,
            "error": f"Binary not found: {binary}"
        }

    log_file = f"{LOG_DIR}/preview-{preview.folder}.log"

    try:
        # Start player_preview as a detached subprocess
        cmd = [
            binary,
            "--input", preview.input_address,
            "--output-dir", preview.output_dir,
            "--api-port", str(preview.api_port)
        ]

        # Open log file for output
        with open(log_file, 'w') as log_f:
            process = subprocess.Popen(
                cmd,
                stdout=log_f,
                stderr=subprocess.STDOUT,
                stdin=subprocess.DEVNULL,
                start_new_session=True  # Detach from parent
            )

        logger.info(f"Started player_preview: PID={process.pid}, port={preview.api_port}")

        # Wait a moment and check if it started
        import time
        time.sleep(0.5)

        if is_preview_running(preview.api_port):
            return {
                "success": True,
                "message": "Preview started",
                "pid": process.pid,
                "api_port": preview.api_port,
                "log_file": log_file
            }
        else:
            # Read log for error info
            log_content = ""
            if os.path.exists(log_file):
                with open(log_file, 'r') as f:
                    log_content = f.read()[:500]

            return {
                "success": False,
                "error": "Failed to start preview",
                "log": log_content
            }

    except Exception as e:
        logger.error(f"Failed to start preview: {e}")
        logger.error(traceback.format_exc())
        return {
            "success": False,
            "error": str(e)
        }


@app.post("/preview/stop/{api_port}")
async def stop_preview(api_port: int):
    """Stop a player_preview by sending a request to its shutdown or just killing it"""

    if not is_preview_running(api_port):
        return {
            "success": True,
            "message": "Preview not running"
        }

    # Find and kill the process listening on that port
    try:
        result = subprocess.run(
            ["fuser", "-k", f"{api_port}/tcp"],
            capture_output=True,
            text=True
        )
        return {
            "success": True,
            "message": f"Stopped preview on port {api_port}"
        }
    except Exception as e:
        return {
            "success": False,
            "error": str(e)
        }


class MediaInfoRequest(BaseModel):
    """Model for media info request"""
    stream_url: str  # e.g., "udp://239.100.0.1:10000" or multicast address


@app.post("/preview/media-info")
async def get_media_info(request: MediaInfoRequest):
    """Get media information using ffprobe"""

    stream_url = request.stream_url

    # If it's just an address:port, assume UDP multicast
    if not stream_url.startswith(('udp://', 'srt://', 'rtmp://', 'http://', 'https://')):
        stream_url = f"udp://@{stream_url}"

    try:
        cmd = [
            "ffprobe",
            "-v", "quiet",
            "-print_format", "json",
            "-show_format",
            "-show_streams",
            "-show_programs",
            "-analyzeduration", "2000000",  # 2 seconds
            "-probesize", "2000000",
            "-i", stream_url
        ]

        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=10
        )

        if result.returncode != 0:
            return {
                "success": False,
                "error": f"FFprobe failed: {result.stderr[:200]}"
            }

        data = json.loads(result.stdout)

        # Parse and simplify the response
        media_info = {
            "success": True,
            "video": None,
            "audio": [],
            "programs": []
        }

        # Extract video info
        for stream in data.get("streams", []):
            if stream.get("codec_type") == "video":
                media_info["video"] = {
                    "codec": stream.get("codec_name", "unknown").upper(),
                    "profile": stream.get("profile", ""),
                    "width": stream.get("width", 0),
                    "height": stream.get("height", 0),
                    "fps": eval(stream.get("r_frame_rate", "0/1")) if "/" in str(stream.get("r_frame_rate", "0")) else float(stream.get("r_frame_rate", 0)),
                    "pix_fmt": stream.get("pix_fmt", ""),
                    "level": stream.get("level", ""),
                    "bitrate": int(stream.get("bit_rate", 0)) if stream.get("bit_rate") else None,
                    "pid": stream.get("id", "")
                }
            elif stream.get("codec_type") == "audio":
                media_info["audio"].append({
                    "codec": stream.get("codec_name", "unknown").upper(),
                    "profile": stream.get("profile", ""),
                    "channels": stream.get("channels", 0),
                    "channel_layout": stream.get("channel_layout", ""),
                    "sample_rate": int(stream.get("sample_rate", 0)),
                    "bitrate": int(stream.get("bit_rate", 0)) if stream.get("bit_rate") else None,
                    "language": stream.get("tags", {}).get("language", "und"),
                    "pid": stream.get("id", "")
                })

        # Extract program info
        for program in data.get("programs", []):
            media_info["programs"].append({
                "id": program.get("program_id", 0),
                "name": program.get("tags", {}).get("service_name", f"Program {program.get('program_id', 0)}")
            })

        return media_info

    except subprocess.TimeoutExpired:
        return {
            "success": False,
            "error": "Timeout waiting for stream data"
        }
    except json.JSONDecodeError as e:
        return {
            "success": False,
            "error": f"Failed to parse ffprobe output: {str(e)}"
        }
    except Exception as e:
        logger.error(f"Media info error: {e}")
        logger.error(traceback.format_exc())
        return {
            "success": False,
            "error": str(e)
        }


class StreamScanRequest(BaseModel):
    """Model for stream scan request"""
    stream_url: str  # e.g., "udp://239.4.4.4:4000" or "239.4.4.4:4000"
    stream_type: Optional[str] = "udp"  # udp, srt, rist, rtmp, hls, http


@app.post("/stream/scan")
async def scan_stream(request: StreamScanRequest):
    """Scan a stream for programs and PIDs using ffprobe

    Returns programs with their associated video and audio PIDs.
    For MPTS (multi-program transport streams), each program will have its own PIDs.
    """

    stream_url = request.stream_url
    stream_type = request.stream_type or "udp"

    # Build proper URL based on type
    if not stream_url.startswith(('udp://', 'srt://', 'rist://', 'rtmp://', 'http://', 'https://')):
        if stream_type == "udp":
            stream_url = f"udp://@{stream_url}"
        elif stream_type == "srt":
            stream_url = f"srt://{stream_url}"
        elif stream_type == "rist":
            stream_url = f"rist://{stream_url}"
        elif stream_type in ("hls", "http", "https"):
            if not stream_url.startswith("http"):
                stream_url = f"http://{stream_url}"

    # Adjust timeout based on type
    timeout = 20 if stream_type in ("hls", "srt", "rist") else 15

    try:
        cmd = [
            "ffprobe",
            "-v", "quiet",
            "-print_format", "json",
            "-show_programs",
            "-show_streams",
            "-analyzeduration", "5000000",  # 5 seconds for better detection
            "-probesize", "5000000",
            "-i", stream_url
        ]

        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=timeout
        )

        if result.returncode != 0:
            return {
                "success": False,
                "error": f"FFprobe failed: {result.stderr[:300] if result.stderr else 'No stream data received'}"
            }

        data = json.loads(result.stdout)

        # Build response with programs and their PIDs
        scan_result = {
            "success": True,
            "programs": [],
            "all_video_pids": [],
            "all_audio_pids": []
        }

        # Process programs - each program has its own streams
        for program in data.get("programs", []):
            program_info = {
                "id": program.get("program_id", 0),
                "name": program.get("tags", {}).get("service_name", f"Program {program.get('program_id', 0)}"),
                "provider": program.get("tags", {}).get("service_provider", ""),
                "pmt_pid": program.get("pmt_pid", 0),
                "pcr_pid": program.get("pcr_pid", 0),
                "video_pids": [],
                "audio_pids": []
            }

            # Process streams within this program
            for stream in program.get("streams", []):
                pid_hex = stream.get("id", "")
                pid = int(pid_hex, 16) if pid_hex.startswith("0x") else int(pid_hex) if pid_hex else 0

                if stream.get("codec_type") == "video":
                    video_info = {
                        "pid": pid,
                        "pid_hex": pid_hex,
                        "codec": stream.get("codec_name", "unknown").upper(),
                        "profile": stream.get("profile", ""),
                        "width": stream.get("width", 0),
                        "height": stream.get("height", 0),
                        "fps": stream.get("r_frame_rate", "0/0"),
                        "pix_fmt": stream.get("pix_fmt", ""),
                        "description": f"{stream.get('codec_name', 'video').upper()} {stream.get('width', 0)}x{stream.get('height', 0)}"
                    }
                    program_info["video_pids"].append(video_info)
                    scan_result["all_video_pids"].append({**video_info, "program_id": program_info["id"]})

                elif stream.get("codec_type") == "audio":
                    audio_info = {
                        "pid": pid,
                        "pid_hex": pid_hex,
                        "codec": stream.get("codec_name", "unknown").upper(),
                        "profile": stream.get("profile", ""),
                        "channels": stream.get("channels", 0),
                        "channel_layout": stream.get("channel_layout", ""),
                        "sample_rate": int(stream.get("sample_rate", 0)),
                        "language": stream.get("tags", {}).get("language", "und"),
                        "description": f"{stream.get('codec_name', 'audio').upper()} {stream.get('tags', {}).get('language', 'und')} {stream.get('channels', 0)}ch"
                    }
                    program_info["audio_pids"].append(audio_info)
                    scan_result["all_audio_pids"].append({**audio_info, "program_id": program_info["id"]})

            scan_result["programs"].append(program_info)

        # If no programs found, fall back to top-level streams (for non-MPTS)
        if not scan_result["programs"]:
            # Create a default program from top-level streams
            default_program = {
                "id": 1,
                "name": "Default Program",
                "provider": "",
                "pmt_pid": 0,
                "pcr_pid": 0,
                "video_pids": [],
                "audio_pids": []
            }

            for stream in data.get("streams", []):
                pid_hex = stream.get("id", "")
                pid = int(pid_hex, 16) if pid_hex.startswith("0x") else int(pid_hex) if pid_hex else 0

                if stream.get("codec_type") == "video":
                    video_info = {
                        "pid": pid,
                        "pid_hex": pid_hex,
                        "codec": stream.get("codec_name", "unknown").upper(),
                        "profile": stream.get("profile", ""),
                        "width": stream.get("width", 0),
                        "height": stream.get("height", 0),
                        "fps": stream.get("r_frame_rate", "0/0"),
                        "pix_fmt": stream.get("pix_fmt", ""),
                        "description": f"{stream.get('codec_name', 'video').upper()} {stream.get('width', 0)}x{stream.get('height', 0)}"
                    }
                    default_program["video_pids"].append(video_info)
                    scan_result["all_video_pids"].append({**video_info, "program_id": 1})

                elif stream.get("codec_type") == "audio":
                    audio_info = {
                        "pid": pid,
                        "pid_hex": pid_hex,
                        "codec": stream.get("codec_name", "unknown").upper(),
                        "profile": stream.get("profile", ""),
                        "channels": stream.get("channels", 0),
                        "channel_layout": stream.get("channel_layout", ""),
                        "sample_rate": int(stream.get("sample_rate", 0)),
                        "language": stream.get("tags", {}).get("language", "und"),
                        "description": f"{stream.get('codec_name', 'audio').upper()} {stream.get('tags', {}).get('language', 'und')} {stream.get('channels', 0)}ch"
                    }
                    default_program["audio_pids"].append(audio_info)
                    scan_result["all_audio_pids"].append({**audio_info, "program_id": 1})

            if default_program["video_pids"] or default_program["audio_pids"]:
                scan_result["programs"].append(default_program)

        scan_result["success"] = len(scan_result["programs"]) > 0
        if not scan_result["success"]:
            scan_result["error"] = "No programs or streams found in source"

        return scan_result

    except subprocess.TimeoutExpired:
        return {
            "success": False,
            "error": f"Timeout waiting for stream data (waited {timeout}s)"
        }
    except json.JSONDecodeError as e:
        return {
            "success": False,
            "error": f"Failed to parse ffprobe output: {str(e)}"
        }
    except Exception as e:
        logger.error(f"Stream scan error: {e}")
        logger.error(traceback.format_exc())
        return {
            "success": False,
            "error": str(e)
        }


# ----------------------------------------------------------------------------
# Transcoder Service Management
# ----------------------------------------------------------------------------

def generate_abr_transcoder_service_file(service_data: TranscoderService, log_file: str) -> str:
    """Generate systemd service file for ABR transcoder with multiple variants"""

    # Build cari-transcoder-abr command
    transcoder_cmd_parts = [
        "/usr/local/bin/cari-transcoder-abr",
        f"--input {service_data.input_address}:{service_data.input_port}"
    ]

    # Add each variant
    for variant in service_data.variants:
        transcoder_cmd_parts.append(
            f'--variant "{variant.width}x{variant.height}:{variant.bitrate}:{variant.video_pid}"'
        )

    # Audio settings
    transcoder_cmd_parts.append(f"--audio-pid {service_data.audio_pid}")
    transcoder_cmd_parts.append(f"--audio-bitrate {service_data.audio_bitrate}")

    # Video codec and preset (common to all variants)
    if service_data.video_codec != "h264":
        transcoder_cmd_parts.append(f"--video-codec {service_data.video_codec}")
    if service_data.video_preset != "superfast":
        transcoder_cmd_parts.append(f"--video-preset {service_data.video_preset}")
    if service_data.keyframe_interval != 60:
        transcoder_cmd_parts.append(f"--keyframe-interval {service_data.keyframe_interval}")

    # Audio codec
    if service_data.audio_codec != "aac":
        transcoder_cmd_parts.append(f"--audio-codec {service_data.audio_codec}")

    # Program number
    if service_data.program_number != 1:
        transcoder_cmd_parts.append(f"--program {service_data.program_number}")

    # Output
    transcoder_cmd_parts.append(f"--output {service_data.output_address}:{service_data.output_port}")

    transcoder_cmd = " ".join(transcoder_cmd_parts)

    # Build tsp monitoring command for all video PIDs plus audio
    # Monitor each variant's video PID
    tsp_monitor_parts = [f"tsp -I ip {service_data.output_address}:{service_data.output_port}"]
    for variant in service_data.variants:
        tsp_monitor_parts.append(f"-P bitrate_monitor --pid {variant.video_pid} --periodic-bitrate 2")
    tsp_monitor_parts.append(f"-P bitrate_monitor --pid {service_data.audio_pid} --periodic-bitrate 2")
    tsp_monitor_parts.append("-O drop")
    tsp_monitor_cmd = " ".join(tsp_monitor_parts)

    description = service_data.description or f"CariTranscoder ABR - {service_data.name}"

    service_content = f"""[Unit]
Description={description}
Documentation=https://github.com/caritechsolutions/caritranscoder
After=network.target
Wants=network-online.target

[Service]
Type=simple
User=root
Group=root

# Main ABR transcoder process with direct UDP output
# tsp monitor starts in background after 2 second delay for bitrate monitoring
ExecStart=/bin/bash -c '(sleep 2 && {tsp_monitor_cmd} >>{log_file} 2>&1) & exec {transcoder_cmd}'
ExecReload=/bin/kill -HUP $MAINPID

# Restart behavior
Restart=always
RestartSec=5
StartLimitIntervalSec=60
StartLimitBurst=5

# Resource limits
LimitNOFILE=65535
LimitNPROC=4096

# Logging
StandardOutput=journal
StandardError=journal
SyslogIdentifier=cari-transcoder-{service_data.id}

[Install]
WantedBy=multi-user.target
"""
    return service_content


def generate_transcoder_service_file(service_data: TranscoderService) -> str:
    """Generate systemd service file for transcoder with tsp CBR output"""

    log_file = f"/var/log/caritrans/transcoder-{service_data.id}.log"

    # Check if ABR mode is enabled
    is_abr = service_data.abr_enabled and service_data.variants and len(service_data.variants) > 0

    if is_abr:
        return generate_abr_transcoder_service_file(service_data, log_file)

    # Build cari-transcoder command - only include essential options
    # The transcoder uses sensible defaults, so we only pass what's needed
    transcoder_cmd_parts = [
        "/usr/local/bin/cari-transcoder",
        f"--input {service_data.input_address}:{service_data.input_port}",
        f"--video-bitrate {service_data.video_bitrate}",
        f"--audio-bitrate {service_data.audio_bitrate}",
        f"--video-pid {service_data.video_pid}",
        f"--audio-pid {service_data.audio_pid}",
        f"--program-number {service_data.program_number}"
    ]

    # Video mode
    if service_data.video_mode != "transcode":
        transcoder_cmd_parts.append(f"--video-mode {service_data.video_mode}")
    else:
        # Only include codec if not default (h264)
        if service_data.video_codec != "h264":
            transcoder_cmd_parts.append(f"--video-codec {service_data.video_codec}")

        # Only include preset if not default (superfast)
        if service_data.video_preset != "superfast":
            transcoder_cmd_parts.append(f"--video-preset {service_data.video_preset}")

        # Only include keyframe interval if not default (60)
        if service_data.keyframe_interval != 60:
            transcoder_cmd_parts.append(f"--keyframe-interval {service_data.keyframe_interval}")

        # x264/x265 specific options - only include if changed from defaults
        if service_data.video_codec in ["h264", "h265"]:
            if service_data.profile != "main":
                transcoder_cmd_parts.append(f"--profile {service_data.profile}")
            if service_data.bframes != 0:
                transcoder_cmd_parts.append(f"--bframes {service_data.bframes}")
            if service_data.ref != 1:
                transcoder_cmd_parts.append(f"--ref {service_data.ref}")
            if service_data.qp_min != 10:
                transcoder_cmd_parts.append(f"--qp-min {service_data.qp_min}")
            if service_data.qp_max != 51:
                transcoder_cmd_parts.append(f"--qp-max {service_data.qp_max}")
            if service_data.vbv_bufsize != 600:
                transcoder_cmd_parts.append(f"--vbv-bufsize {service_data.vbv_bufsize}")
            if service_data.video_threads != 0:
                transcoder_cmd_parts.append(f"--threads {service_data.video_threads}")

            # Boolean flags - only include if explicitly enabled/disabled from default
            if service_data.sliced_threads:
                transcoder_cmd_parts.append("--sliced-threads")
            if not service_data.cabac:
                transcoder_cmd_parts.append("--no-cabac")
            if service_data.trellis:
                transcoder_cmd_parts.append("--trellis")
            if not service_data.aud:
                transcoder_cmd_parts.append("--no-aud")
            if service_data.intra_refresh:
                transcoder_cmd_parts.append("--intra-refresh")
            if service_data.interlaced:
                transcoder_cmd_parts.append("--interlaced")
            if service_data.psy_tune:
                transcoder_cmd_parts.append(f"--psy-tune {service_data.psy_tune}")
            if service_data.x264_opts:
                transcoder_cmd_parts.append(f'--x264-opts "{service_data.x264_opts}"')

    # Scaling options - only if scaling is enabled
    if service_data.scaling_enabled:
        transcoder_cmd_parts.append(f"--scale {service_data.scale_width}x{service_data.scale_height}")
        if service_data.scale_method != 1:
            transcoder_cmd_parts.append(f"--scale-method {service_data.scale_method}")
        if service_data.scale_threads != 0:
            transcoder_cmd_parts.append(f"--scale-threads {service_data.scale_threads}")
        if service_data.add_borders:
            transcoder_cmd_parts.append("--add-borders")
        if service_data.deinterlace:
            transcoder_cmd_parts.append("--deinterlace")

    # Audio mode
    if service_data.audio_mode != "transcode":
        transcoder_cmd_parts.append(f"--audio-mode {service_data.audio_mode}")
    else:
        # Only include codec if not default (aac)
        if service_data.audio_codec != "aac":
            transcoder_cmd_parts.append(f"--audio-codec {service_data.audio_codec}")

        # Only include channels/samplerate if not default
        if service_data.audio_channels != 2:
            transcoder_cmd_parts.append(f"--audio-channels {service_data.audio_channels}")
        if service_data.audio_samplerate != 48000:
            transcoder_cmd_parts.append(f"--audio-samplerate {service_data.audio_samplerate}")

    transcoder_cmd = " ".join(transcoder_cmd_parts)

    # Add UDP output to transcoder command
    transcoder_cmd += f" --udp-host {service_data.output_address} --udp-port {service_data.output_port}"

    # Build tsp monitoring command - reads from output stream for bitrate monitoring
    tsp_monitor_cmd = (
        f"tsp -I ip {service_data.output_address}:{service_data.output_port} "
        f"-P bitrate_monitor --pid {service_data.video_pid} --periodic-bitrate 2 "
        f"-P bitrate_monitor --pid {service_data.audio_pid} --periodic-bitrate 2 "
        f"-O drop"
    )

    description = service_data.description or f"CariTranscoder - {service_data.name}"

    service_content = f"""[Unit]
Description={description}
Documentation=https://github.com/caritechsolutions/caritranscoder
After=network.target
Wants=network-online.target

[Service]
Type=simple
User=root
Group=root

# Main transcoder process with direct UDP output
# tsp monitor starts in background after 2 second delay for bitrate monitoring
ExecStart=/bin/bash -c '(sleep 2 && {tsp_monitor_cmd} >>{log_file} 2>&1) & exec {transcoder_cmd}'
ExecReload=/bin/kill -HUP $MAINPID

# Restart behavior
Restart=always
RestartSec=5
StartLimitIntervalSec=60
StartLimitBurst=5

# Resource limits
LimitNOFILE=65535
LimitNPROC=4096

# Logging
StandardOutput=journal
StandardError=journal
SyslogIdentifier=cari-transcoder-{service_data.id}

[Install]
WantedBy=multi-user.target
"""
    return service_content


@app.post("/transcoder/create")
async def create_transcoder_service(service: TranscoderService):
    """Create a systemd service file for transcoder"""

    service_name = f"cari-transcoder@{service.id}"
    service_file = f"{SYSTEMD_DIR}/{service_name}.service"

    try:
        # Ensure log directory exists
        log_dir = "/var/log/caritrans"
        os.makedirs(log_dir, exist_ok=True)
        os.chmod(log_dir, 0o755)

        # Generate service content
        service_content = generate_transcoder_service_file(service)

        # Write the service file
        with open(service_file, 'w') as f:
            f.write(service_content)

        logger.info(f"Created transcoder service file: {service_file}")

        # Reload systemd
        daemon_reload()

        return {
            "success": True,
            "service_name": service_name,
            "service_file": service_file,
            "message": f"Transcoder service created: {service_file}"
        }
    except Exception as e:
        logger.error(f"Failed to create transcoder service: {e}")
        logger.error(traceback.format_exc())
        raise HTTPException(status_code=500, detail=str(e))


@app.post("/transcoder/delete")
async def delete_transcoder_service(data: dict = Body(...)):
    """Delete a transcoder service"""

    transcoder_id = data.get("id")
    if not transcoder_id:
        raise HTTPException(status_code=400, detail="Missing transcoder ID")

    service_name = f"cari-transcoder@{transcoder_id}.service"
    service_file = f"{SYSTEMD_DIR}/{service_name}"

    try:
        # Stop and disable the service first
        run_systemctl("stop", service_name)
        run_systemctl("disable", service_name)

        # Remove the service file
        if os.path.exists(service_file):
            os.remove(service_file)
            logger.info(f"Deleted transcoder service file: {service_file}")

        # Reload systemd
        daemon_reload()

        return {
            "success": True,
            "message": f"Transcoder service {service_name} deleted"
        }
    except Exception as e:
        logger.error(f"Failed to delete transcoder service: {e}")
        raise HTTPException(status_code=500, detail=str(e))


@app.post("/transcoder/{transcoder_id}/start")
async def start_transcoder(transcoder_id: str):
    """Start a transcoder service"""
    service_name = f"cari-transcoder@{transcoder_id}.service"

    service_file = f"{SYSTEMD_DIR}/{service_name}"
    if not os.path.exists(service_file):
        return {
            "success": False,
            "service": service_name,
            "error": f"Service file not found: {service_file}"
        }

    result = run_systemctl("start", service_name)
    return {
        "success": result.get("success", False),
        "service": service_name,
        **result
    }


@app.post("/transcoder/{transcoder_id}/stop")
async def stop_transcoder(transcoder_id: str):
    """Stop a transcoder service"""
    service_name = f"cari-transcoder@{transcoder_id}.service"
    result = run_systemctl("stop", service_name)
    return {
        "success": result.get("success", False),
        "service": service_name,
        **result
    }


@app.get("/transcoder/{transcoder_id}/status")
async def transcoder_status(transcoder_id: str):
    """Get transcoder service status"""
    service_name = f"cari-transcoder@{transcoder_id}.service"
    return get_service_status(service_name)


# ----------------------------------------------------------------------------
# System Operations
# ----------------------------------------------------------------------------

@app.post("/systemd/daemon-reload")
async def do_daemon_reload():
    """Reload systemd daemon"""
    result = daemon_reload()
    return {
        "action": "daemon-reload",
        **result
    }


# ============================================================================
# Main Entry Point
# ============================================================================

if __name__ == "__main__":
    import uvicorn
    logger.info(f"Starting CariTranscoder API on port {API_PORT}")
    uvicorn.run(app, host="127.0.0.1", port=API_PORT)
