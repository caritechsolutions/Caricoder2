<?php
/**
 * CariTranscoder - Logs Viewer
 * Copyright (c) 2024 CariTech Solutions
 */

define('CARITRANS', true);
require_once __DIR__ . '/../includes/config.php';
require_once __DIR__ . '/../includes/auth.php';
require_once __DIR__ . '/../includes/functions.php';

auth_require_login();

$page_title = 'Logs';
include __DIR__ . '/../templates/header.php';
?>

<div class="container-fluid py-4">
    <div class="d-flex justify-content-between align-items-center mb-4">
        <h2><i class="bi bi-journal-text me-2"></i>System Logs</h2>
        <div class="btn-group">
            <button class="btn btn-outline-secondary" onclick="refreshLogs()">
                <i class="bi bi-arrow-clockwise"></i> Refresh
            </button>
            <button class="btn btn-outline-secondary" onclick="downloadLogs()">
                <i class="bi bi-download"></i> Download
            </button>
            <button class="btn btn-outline-danger" onclick="clearLogs()">
                <i class="bi bi-trash"></i> Clear
            </button>
        </div>
    </div>

    <div class="card">
        <div class="card-header">
            <div class="row align-items-center">
                <div class="col-md-3">
                    <select class="form-select form-select-sm" id="logSource" onchange="filterLogs()">
                        <option value="all">All Sources</option>
                        <option value="cari-input">Inputs</option>
                        <option value="cari-transcoder">Transcoders</option>
                        <option value="cari-mux">Muxers</option>
                        <option value="cari-output">Outputs</option>
                        <option value="system">System</option>
                    </select>
                </div>
                <div class="col-md-3">
                    <select class="form-select form-select-sm" id="logLevel" onchange="filterLogs()">
                        <option value="all">All Levels</option>
                        <option value="debug">Debug</option>
                        <option value="info">Info</option>
                        <option value="warning">Warning</option>
                        <option value="error">Error</option>
                    </select>
                </div>
                <div class="col-md-4">
                    <input type="text" class="form-control form-control-sm" id="logSearch"
                           placeholder="Search logs..." onkeyup="filterLogs()">
                </div>
                <div class="col-md-2">
                    <div class="form-check">
                        <input class="form-check-input" type="checkbox" id="autoRefresh" checked>
                        <label class="form-check-label" for="autoRefresh">Auto-refresh</label>
                    </div>
                </div>
            </div>
        </div>
        <div class="card-body p-0">
            <div class="log-viewer" id="logViewer">
                <pre id="logContent" class="log-content m-0 p-3"></pre>
            </div>
        </div>
    </div>
</div>

<style>
.log-viewer {
    height: calc(100vh - 300px);
    overflow: auto;
    background: #1e1e1e;
}
.log-content {
    font-family: 'Consolas', 'Monaco', monospace;
    font-size: 0.85rem;
    color: #d4d4d4;
    white-space: pre-wrap;
    word-wrap: break-word;
}
.log-line { margin: 0; padding: 2px 0; }
.log-line:hover { background: rgba(255,255,255,0.05); }
.log-debug { color: #6c757d; }
.log-info { color: #17a2b8; }
.log-warning { color: #ffc107; }
.log-error { color: #dc3545; }
.log-timestamp { color: #6c757d; }
.log-source { color: #28a745; }
</style>

<script>
let autoRefreshInterval = null;
let lastLogCount = 0;

function fetchLogs() {
    const source = document.getElementById('logSource').value;
    const level = document.getElementById('logLevel').value;

    fetch(`api/logs.php?source=${source}&level=${level}`)
        .then(r => r.json())
        .then(data => {
            if (data.logs) {
                renderLogs(data.logs);
            }
        })
        .catch(err => console.error('Failed to fetch logs:', err));
}

function renderLogs(logs) {
    const container = document.getElementById('logContent');
    const search = document.getElementById('logSearch').value.toLowerCase();

    let html = '';
    logs.forEach(log => {
        if (search && !log.message.toLowerCase().includes(search)) return;

        const levelClass = `log-${log.level}`;
        html += `<div class="log-line ${levelClass}">`;
        html += `<span class="log-timestamp">[${log.timestamp}]</span> `;
        html += `<span class="log-source">[${log.source}]</span> `;
        html += `<span class="log-level">[${log.level.toUpperCase()}]</span> `;
        html += `${escapeHtml(log.message)}`;
        html += `</div>`;
    });

    container.innerHTML = html || '<div class="text-muted p-3">No logs found</div>';

    // Auto-scroll to bottom if new logs
    if (logs.length > lastLogCount) {
        const viewer = document.getElementById('logViewer');
        viewer.scrollTop = viewer.scrollHeight;
    }
    lastLogCount = logs.length;
}

function escapeHtml(text) {
    const div = document.createElement('div');
    div.textContent = text;
    return div.innerHTML;
}

function filterLogs() {
    fetchLogs();
}

function refreshLogs() {
    fetchLogs();
}

function downloadLogs() {
    const source = document.getElementById('logSource').value;
    window.location.href = `api/logs.php?action=download&source=${source}`;
}

function clearLogs() {
    if (confirm('Are you sure you want to clear all logs?')) {
        fetch('api/logs.php?action=clear', { method: 'POST' })
            .then(r => r.json())
            .then(data => {
                if (data.success) fetchLogs();
            });
    }
}

// Initialize
document.addEventListener('DOMContentLoaded', function() {
    fetchLogs();

    // Auto-refresh toggle
    document.getElementById('autoRefresh').addEventListener('change', function() {
        if (this.checked) {
            autoRefreshInterval = setInterval(fetchLogs, 5000);
        } else {
            clearInterval(autoRefreshInterval);
        }
    });

    // Start auto-refresh
    autoRefreshInterval = setInterval(fetchLogs, 5000);
});
</script>

<?php include __DIR__ . '/../templates/footer.php'; ?>
