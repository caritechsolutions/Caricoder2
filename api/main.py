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
