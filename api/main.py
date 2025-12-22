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
