/**
 * CariTranscoder - Main JavaScript
 * Copyright (c) 2024 CariTech Solutions
 */

// Global configuration
const CT = {
    refreshInterval: 5000,
    wsReconnectDelay: 3000,
    apiBase: 'api/',
    ws: null,
    wsConnected: false
};

/**
 * Show toast notification
 */
function showToast(message, type = 'info', duration = 5000) {
    const container = document.getElementById('toast-container');
    const id = 'toast-' + Date.now();

    const bgClass = {
        'success': 'bg-success',
        'error': 'bg-danger',
        'warning': 'bg-warning',
        'info': 'bg-info'
    }[type] || 'bg-info';

    const icon = {
        'success': 'check-circle',
        'error': 'exclamation-circle',
        'warning': 'exclamation-triangle',
        'info': 'info-circle'
    }[type] || 'info-circle';

    const html = `
        <div id="${id}" class="toast" role="alert" aria-live="assertive" aria-atomic="true">
            <div class="toast-header ${bgClass} text-white">
                <i class="bi bi-${icon} me-2"></i>
                <strong class="me-auto">${type.charAt(0).toUpperCase() + type.slice(1)}</strong>
                <small>just now</small>
                <button type="button" class="btn-close btn-close-white" data-bs-dismiss="toast"></button>
            </div>
            <div class="toast-body">
                ${message}
            </div>
        </div>
    `;

    container.insertAdjacentHTML('beforeend', html);

    const toastEl = document.getElementById(id);
    const toast = new bootstrap.Toast(toastEl, { delay: duration });
    toast.show();

    toastEl.addEventListener('hidden.bs.toast', () => {
        toastEl.remove();
    });
}

/**
 * Show inline alert
 */
function showAlert(message, type = 'info') {
    const container = document.getElementById('alerts-container');
    const id = 'alert-' + Date.now();

    const html = `
        <div id="${id}" class="alert alert-${type} alert-dismissible fade show" role="alert">
            ${message}
            <button type="button" class="btn-close" data-bs-dismiss="alert"></button>
        </div>
    `;

    container.insertAdjacentHTML('beforeend', html);

    // Auto-dismiss after 10 seconds
    setTimeout(() => {
        const alert = document.getElementById(id);
        if (alert) {
            bootstrap.Alert.getOrCreateInstance(alert).close();
        }
    }, 10000);
}

/**
 * API request helper
 */
async function apiRequest(endpoint, method = 'GET', data = null) {
    const options = {
        method: method,
        headers: {
            'Content-Type': 'application/json',
            'X-CSRF-Token': csrfToken
        }
    };

    if (data && method !== 'GET') {
        options.body = JSON.stringify(data);
    }

    try {
        const response = await fetch(CT.apiBase + endpoint, options);
        const json = await response.json();

        if (!response.ok) {
            throw new Error(json.message || 'Request failed');
        }

        return json;
    } catch (error) {
        console.error('API Error:', error);
        throw error;
    }
}

/**
 * Confirm action modal
 */
function confirmAction(title, message, callback) {
    const modal = document.getElementById('confirmModal');
    const bsModal = new bootstrap.Modal(modal);

    document.getElementById('confirmModalTitle').textContent = title;
    document.getElementById('confirmModalBody').textContent = message;

    const okBtn = document.getElementById('confirmModalOk');

    // Remove old listeners
    const newOkBtn = okBtn.cloneNode(true);
    okBtn.parentNode.replaceChild(newOkBtn, okBtn);

    newOkBtn.addEventListener('click', () => {
        bsModal.hide();
        callback();
    });

    bsModal.show();
}

/**
 * Start a pipeline/service
 */
async function startPipeline(id) {
    try {
        const result = await apiRequest('pipelines.php?action=start&id=' + encodeURIComponent(id), 'POST');
        if (result.success) {
            showToast('Pipeline started successfully', 'success');
            refreshPipelines();
        } else {
            showToast(result.message || 'Failed to start pipeline', 'error');
        }
    } catch (error) {
        showToast('Error: ' + error.message, 'error');
    }
}

/**
 * Stop a pipeline/service
 */
async function stopPipeline(id) {
    confirmAction('Stop Pipeline', 'Are you sure you want to stop this pipeline?', async () => {
        try {
            const result = await apiRequest('pipelines.php?action=stop&id=' + encodeURIComponent(id), 'POST');
            if (result.success) {
                showToast('Pipeline stopped', 'success');
                refreshPipelines();
            } else {
                showToast(result.message || 'Failed to stop pipeline', 'error');
            }
        } catch (error) {
            showToast('Error: ' + error.message, 'error');
        }
    });
}

/**
 * Delete a pipeline/service
 */
async function deletePipeline(id) {
    confirmAction('Delete Pipeline', 'Are you sure you want to delete this pipeline? This action cannot be undone.', async () => {
        try {
            const result = await apiRequest('pipelines.php?action=delete&id=' + encodeURIComponent(id), 'DELETE');
            if (result.success) {
                showToast('Pipeline deleted', 'success');
                refreshPipelines();
            } else {
                showToast(result.message || 'Failed to delete pipeline', 'error');
            }
        } catch (error) {
            showToast('Error: ' + error.message, 'error');
        }
    });
}

/**
 * Start a specific service
 */
async function startService(type, id) {
    try {
        const result = await apiRequest(`services.php?action=start&type=${type}&id=${encodeURIComponent(id)}`, 'POST');
        if (result.success) {
            showToast(`${type} started successfully`, 'success');
            location.reload();
        } else {
            showToast(result.message || `Failed to start ${type}`, 'error');
        }
    } catch (error) {
        showToast('Error: ' + error.message, 'error');
    }
}

/**
 * Stop a specific service
 */
async function stopService(type, id) {
    confirmAction(`Stop ${type}`, `Are you sure you want to stop this ${type}?`, async () => {
        try {
            const result = await apiRequest(`services.php?action=stop&type=${type}&id=${encodeURIComponent(id)}`, 'POST');
            if (result.success) {
                showToast(`${type} stopped`, 'success');
                location.reload();
            } else {
                showToast(result.message || `Failed to stop ${type}`, 'error');
            }
        } catch (error) {
            showToast('Error: ' + error.message, 'error');
        }
    });
}

/**
 * Delete a service configuration
 */
async function deleteService(type, id) {
    confirmAction(`Delete ${type}`, `Are you sure you want to delete this ${type}? This will remove the configuration file.`, async () => {
        try {
            const result = await apiRequest(`services.php?action=delete&type=${type}&id=${encodeURIComponent(id)}`, 'DELETE');
            if (result.success) {
                showToast(`${type} deleted`, 'success');
                location.reload();
            } else {
                showToast(result.message || `Failed to delete ${type}`, 'error');
            }
        } catch (error) {
            showToast('Error: ' + error.message, 'error');
        }
    });
}

/**
 * Refresh pipeline list
 */
async function refreshPipelines() {
    try {
        const result = await apiRequest('pipelines.php?action=list');
        if (result.pipelines) {
            updatePipelineTable(result.pipelines);
        }
    } catch (error) {
        console.error('Failed to refresh pipelines:', error);
    }
}

/**
 * Update pipeline table
 */
function updatePipelineTable(pipelines) {
    const tbody = document.getElementById('pipeline-list');
    if (!tbody) return;

    if (pipelines.length === 0) {
        tbody.innerHTML = `
            <tr>
                <td colspan="7" class="text-center py-4 text-muted">
                    No active pipelines. <a href="pipeline-builder.php">Create one</a>
                </td>
            </tr>
        `;
        return;
    }

    tbody.innerHTML = pipelines.map(p => `
        <tr data-pipeline-id="${escapeHtml(p.id)}">
            <td>
                <strong>${escapeHtml(p.name)}</strong><br>
                <small class="text-muted">${escapeHtml(p.id)}</small>
            </td>
            <td>
                <span class="badge bg-primary">${escapeHtml(p.input_type)}</span><br>
                <small>${escapeHtml(p.input_source)}</small>
            </td>
            <td>
                ${p.transcode_enabled
                    ? `<span class="badge bg-success">${escapeHtml(p.video_codec)}</span>`
                    : '<span class="badge bg-secondary">Passthrough</span>'}
            </td>
            <td>
                <span class="badge bg-info">${escapeHtml(p.output_type)}</span><br>
                <small>${escapeHtml(p.output_dest)}</small>
            </td>
            <td>
                <span class="bitrate">${formatBitrate(p.bitrate)}</span>
            </td>
            <td>
                <span class="badge status-badge status-${p.status}">${capitalize(p.status)}</span>
            </td>
            <td>
                <div class="btn-group btn-group-sm">
                    ${p.status === 'running'
                        ? `<button class="btn btn-outline-warning" onclick="stopPipeline('${p.id}')" title="Stop">
                               <i class="bi bi-stop-fill"></i>
                           </button>`
                        : `<button class="btn btn-outline-success" onclick="startPipeline('${p.id}')" title="Start">
                               <i class="bi bi-play-fill"></i>
                           </button>`}
                    <a href="pipeline-edit.php?id=${encodeURIComponent(p.id)}" class="btn btn-outline-secondary" title="Edit">
                        <i class="bi bi-gear"></i>
                    </a>
                    <button class="btn btn-outline-danger" onclick="deletePipeline('${p.id}')" title="Delete">
                        <i class="bi bi-trash"></i>
                    </button>
                </div>
            </td>
        </tr>
    `).join('');
}

/**
 * Format bitrate for display
 */
function formatBitrate(bps) {
    if (bps >= 1000000000) {
        return (bps / 1000000000).toFixed(2) + ' Gbps';
    } else if (bps >= 1000000) {
        return (bps / 1000000).toFixed(2) + ' Mbps';
    } else if (bps >= 1000) {
        return (bps / 1000).toFixed(2) + ' Kbps';
    }
    return bps + ' bps';
}

/**
 * Format bytes for display
 */
function formatBytes(bytes) {
    const units = ['B', 'KB', 'MB', 'GB', 'TB'];
    let i = 0;
    while (bytes >= 1024 && i < units.length - 1) {
        bytes /= 1024;
        i++;
    }
    return bytes.toFixed(2) + ' ' + units[i];
}

/**
 * Capitalize string
 */
function capitalize(str) {
    return str.charAt(0).toUpperCase() + str.slice(1);
}

/**
 * Escape HTML
 */
function escapeHtml(text) {
    const div = document.createElement('div');
    div.textContent = text;
    return div.innerHTML;
}

/**
 * WebSocket connection for real-time updates
 */
function initWebSocket() {
    const wsProtocol = location.protocol === 'https:' ? 'wss:' : 'ws:';
    const wsUrl = `${wsProtocol}//${location.hostname}:8081`;

    try {
        CT.ws = new WebSocket(wsUrl);

        CT.ws.onopen = () => {
            CT.wsConnected = true;
            updateConnectionStatus(true);
            console.log('WebSocket connected');
        };

        CT.ws.onclose = () => {
            CT.wsConnected = false;
            updateConnectionStatus(false);
            console.log('WebSocket disconnected, reconnecting...');
            setTimeout(initWebSocket, CT.wsReconnectDelay);
        };

        CT.ws.onerror = (error) => {
            console.error('WebSocket error:', error);
        };

        CT.ws.onmessage = (event) => {
            try {
                const data = JSON.parse(event.data);
                handleWebSocketMessage(data);
            } catch (e) {
                console.error('Failed to parse WebSocket message:', e);
            }
        };
    } catch (error) {
        console.error('WebSocket init error:', error);
        setTimeout(initWebSocket, CT.wsReconnectDelay);
    }
}

/**
 * Handle WebSocket messages
 */
function handleWebSocketMessage(data) {
    switch (data.type) {
        case 'stats':
            updateSystemStats(data.stats);
            break;
        case 'pipeline_status':
            updatePipelineStatus(data.pipeline);
            break;
        case 'alert':
            showToast(data.message, data.level);
            break;
    }
}

/**
 * Update system stats display
 */
function updateSystemStats(stats) {
    // CPU
    const cpuBar = document.getElementById('cpu-bar');
    if (cpuBar) {
        cpuBar.style.width = stats.cpu_percent + '%';
        cpuBar.textContent = stats.cpu_percent + '%';
    }

    const cpuLoad = document.getElementById('cpu-load');
    if (cpuLoad && stats.load_avg) {
        cpuLoad.textContent = stats.load_avg.join(' / ');
    }

    // Memory
    const memBar = document.getElementById('mem-bar');
    if (memBar) {
        memBar.style.width = stats.mem_percent + '%';
        memBar.textContent = stats.mem_percent + '%';
    }

    // Network
    const netRx = document.getElementById('net-rx');
    if (netRx) {
        netRx.textContent = formatBitrate(stats.net_rx);
    }

    const netTx = document.getElementById('net-tx');
    if (netTx) {
        netTx.textContent = formatBitrate(stats.net_tx);
    }
}

/**
 * Update connection status indicator
 */
function updateConnectionStatus(connected) {
    const indicator = document.getElementById('status-indicator');
    const text = document.getElementById('status-text');

    if (indicator) {
        indicator.className = 'status-dot ' + (connected ? 'status-online' : 'status-offline');
    }
    if (text) {
        text.textContent = connected ? 'Online' : 'Offline';
    }
}

/**
 * Initialize on page load
 */
document.addEventListener('DOMContentLoaded', () => {
    // Initialize tooltips
    const tooltipTriggerList = document.querySelectorAll('[data-bs-toggle="tooltip"]');
    tooltipTriggerList.forEach(el => new bootstrap.Tooltip(el));

    // Initialize WebSocket (disabled for now, enable when server is ready)
    // initWebSocket();

    // Poll for stats if WebSocket not available
    if (!CT.wsConnected) {
        setInterval(async () => {
            try {
                const stats = await apiRequest('stats.php');
                if (stats) {
                    updateSystemStats(stats);
                }
            } catch (e) {
                // Silent fail for polling
            }
        }, CT.refreshInterval);
    }
});
