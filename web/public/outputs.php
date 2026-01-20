<?php
/**
 * CariTranscoder - Outputs Management
 * Copyright (c) 2024 CariTech Solutions
 */

define('CARITRANS', true);
require_once __DIR__ . '/../includes/config.php';
require_once __DIR__ . '/../includes/auth.php';
require_once __DIR__ . '/../includes/functions.php';

auth_require_login();

$outputs = get_service_list('outputs');
$page_title = 'Outputs';
include __DIR__ . '/../templates/header.php';
?>

<div class="container-fluid py-4">
    <div class="d-flex justify-content-between align-items-center mb-4">
        <h2><i class="bi bi-upload me-2"></i>Outputs</h2>
        <button class="btn btn-primary" data-bs-toggle="modal" data-bs-target="#addOutputModal">
            <i class="bi bi-plus-lg me-1"></i>Add Output
        </button>
    </div>

    <!-- Outputs Table -->
    <div class="card">
        <div class="card-body p-0">
            <div class="table-responsive">
                <table class="table table-hover mb-0" id="outputs-table">
                    <thead class="table-dark">
                        <tr>
                            <th style="width: 40px;"></th>
                            <th>Name</th>
                            <th>UDP Input</th>
                            <th>SRT Output</th>
                            <th>Max Clients</th>
                            <th>Status</th>
                            <th style="width: 220px;">Actions</th>
                        </tr>
                    </thead>
                    <tbody>
                        <?php if (empty($outputs)): ?>
                        <tr>
                            <td colspan="7" class="text-center py-5">
                                <i class="bi bi-upload text-muted" style="font-size: 3rem;"></i>
                                <h5 class="mt-3">No Outputs</h5>
                                <p class="text-muted">Create an output to send your streams via SRT.</p>
                                <button class="btn btn-primary" data-bs-toggle="modal" data-bs-target="#addOutputModal">
                                    <i class="bi bi-plus-lg me-1"></i>Add Output
                                </button>
                            </td>
                        </tr>
                        <?php else: ?>
                        <?php foreach ($outputs as $output): ?>
                        <?php
                        $input_addr = $output['input']['address'] ?? '';
                        $input_port = $output['input']['port'] ?? '';
                        $srt_addr = $output['destination_srt']['listen_address'] ?? '0.0.0.0';
                        $srt_port = $output['destination_srt']['listen_port'] ?? '';
                        $max_clients = $output['destination_srt']['max_clients'] ?? '10';
                        ?>
                        <tr data-id="<?php echo htmlspecialchars($output['id']); ?>">
                            <td>
                                <span class="status-dot status-<?php echo $output['status'] ?? 'stopped'; ?>"></span>
                            </td>
                            <td>
                                <strong><?php echo htmlspecialchars($output['name']); ?></strong>
                                <br><small class="text-muted"><?php echo htmlspecialchars($output['id']); ?></small>
                            </td>
                            <td>
                                <code><?php
                                if ($input_addr && $input_port) {
                                    echo htmlspecialchars("{$input_addr}:{$input_port}");
                                } elseif ($input_port) {
                                    echo htmlspecialchars("*:{$input_port}");
                                } else {
                                    echo '<span class="text-muted">Not set</span>';
                                }
                                ?></code>
                            </td>
                            <td>
                                <code><?php echo htmlspecialchars("{$srt_addr}:{$srt_port}"); ?></code>
                            </td>
                            <td>
                                <span class="badge bg-info"><?php echo htmlspecialchars($max_clients); ?></span>
                            </td>
                            <td>
                                <span class="badge bg-<?php echo ($output['status'] ?? 'stopped') === 'running' ? 'success' : 'secondary'; ?>">
                                    <?php echo ucfirst($output['status'] ?? 'stopped'); ?>
                                </span>
                            </td>
                            <td>
                                <div class="btn-group btn-group-sm">
                                    <?php if (($output['status'] ?? 'stopped') === 'running'): ?>
                                    <button class="btn btn-outline-warning" onclick="stopOutput('<?php echo $output['id']; ?>')" title="Stop">
                                        <i class="bi bi-stop-fill"></i>
                                    </button>
                                    <?php else: ?>
                                    <button class="btn btn-outline-success" onclick="startOutput('<?php echo $output['id']; ?>')" title="Start">
                                        <i class="bi bi-play-fill"></i>
                                    </button>
                                    <?php endif; ?>
                                    <button class="btn btn-outline-info" onclick="showClientsModal('<?php echo $output['id']; ?>', '<?php echo htmlspecialchars($output['name']); ?>')" title="Clients">
                                        <i class="bi bi-people"></i>
                                    </button>
                                    <button class="btn btn-outline-secondary" onclick="editOutput('<?php echo $output['id']; ?>')" title="Edit">
                                        <i class="bi bi-pencil"></i>
                                    </button>
                                    <button class="btn btn-outline-danger" onclick="deleteOutput('<?php echo $output['id']; ?>')" title="Delete">
                                        <i class="bi bi-trash"></i>
                                    </button>
                                </div>
                            </td>
                        </tr>
                        <?php endforeach; ?>
                        <?php endif; ?>
                    </tbody>
                </table>
            </div>
        </div>
    </div>
</div>

<!-- Clients Modal -->
<div class="modal fade" id="clientsModal" tabindex="-1">
    <div class="modal-dialog modal-xl">
        <div class="modal-content">
            <div class="modal-header">
                <h5 class="modal-title"><i class="bi bi-people me-2"></i>SRT Clients: <span id="clientsOutputName"></span></h5>
                <button type="button" class="btn-close" data-bs-dismiss="modal"></button>
            </div>
            <div class="modal-body">
                <input type="hidden" id="clientsOutputId">

                <!-- Summary -->
                <div class="row mb-4">
                    <div class="col-md-4">
                        <div class="card bg-light">
                            <div class="card-body text-center">
                                <h3 class="mb-0"><span id="clientCount">0</span> / <span id="maxClients">10</span></h3>
                                <small class="text-muted">Connected Clients</small>
                            </div>
                        </div>
                    </div>
                    <div class="col-md-8">
                        <div class="d-flex justify-content-end align-items-center h-100">
                            <button class="btn btn-outline-secondary" onclick="refreshClients()">
                                <i class="bi bi-arrow-clockwise"></i> Refresh
                            </button>
                        </div>
                    </div>
                </div>

                <!-- Clients List -->
                <div class="card">
                    <div class="card-header">
                        <h6 class="mb-0">Connected Clients</h6>
                    </div>
                    <div class="card-body p-0">
                        <div class="clients-list" style="max-height: 400px; overflow-y: auto;">
                            <table class="table table-hover mb-0" id="clientsTable">
                                <thead class="table-light sticky-top">
                                    <tr>
                                        <th>Slot</th>
                                        <th>Address</th>
                                        <th>Duration</th>
                                        <th>RTT</th>
                                        <th>Bandwidth</th>
                                        <th>Lost/Retrans</th>
                                        <th>Actions</th>
                                    </tr>
                                </thead>
                                <tbody id="clientsTableBody">
                                    <tr>
                                        <td colspan="7" class="text-center py-4 text-muted">
                                            <div class="spinner-border spinner-border-sm me-2"></div>
                                            Loading clients...
                                        </td>
                                    </tr>
                                </tbody>
                            </table>
                        </div>
                    </div>
                </div>

                <!-- Client Details (shown when a client is selected) -->
                <div id="clientDetails" class="mt-4 d-none">
                    <div class="card border-primary">
                        <div class="card-header bg-primary text-white d-flex justify-content-between align-items-center">
                            <h6 class="mb-0"><i class="bi bi-person-badge me-2"></i>Client Details: <span id="detailClientAddr"></span></h6>
                            <button type="button" class="btn-close btn-close-white" onclick="hideClientDetails()"></button>
                        </div>
                        <div class="card-body">
                            <div class="row">
                                <div class="col-md-3 mb-3">
                                    <small class="text-muted">Slot</small>
                                    <div class="fw-bold" id="detailSlot">-</div>
                                </div>
                                <div class="col-md-3 mb-3">
                                    <small class="text-muted">Connected Duration</small>
                                    <div class="fw-bold" id="detailDuration">-</div>
                                </div>
                                <div class="col-md-3 mb-3">
                                    <small class="text-muted">RTT</small>
                                    <div class="fw-bold" id="detailRtt">-</div>
                                </div>
                                <div class="col-md-3 mb-3">
                                    <small class="text-muted">Negotiated Latency</small>
                                    <div class="fw-bold" id="detailLatency">-</div>
                                </div>
                            </div>
                            <div class="row">
                                <div class="col-md-3 mb-3">
                                    <small class="text-muted">Send Rate</small>
                                    <div class="fw-bold" id="detailSendRate">-</div>
                                </div>
                                <div class="col-md-3 mb-3">
                                    <small class="text-muted">Est. Bandwidth</small>
                                    <div class="fw-bold" id="detailBandwidth">-</div>
                                </div>
                                <div class="col-md-3 mb-3">
                                    <small class="text-muted">Packets Sent</small>
                                    <div class="fw-bold" id="detailPacketsSent">-</div>
                                </div>
                                <div class="col-md-3 mb-3">
                                    <small class="text-muted">Send Errors</small>
                                    <div class="fw-bold" id="detailSendErrors">-</div>
                                </div>
                            </div>
                            <div class="row">
                                <div class="col-md-3 mb-3">
                                    <small class="text-muted">Packets Lost</small>
                                    <div class="fw-bold text-danger" id="detailLost">-</div>
                                </div>
                                <div class="col-md-3 mb-3">
                                    <small class="text-muted">Retransmitted</small>
                                    <div class="fw-bold text-warning" id="detailRetrans">-</div>
                                </div>
                                <div class="col-md-3 mb-3">
                                    <small class="text-muted">Dropped</small>
                                    <div class="fw-bold" id="detailDropped">-</div>
                                </div>
                                <div class="col-md-3 mb-3">
                                    <small class="text-muted">In Flight</small>
                                    <div class="fw-bold" id="detailFlight">-</div>
                                </div>
                            </div>
                            <div class="row">
                                <div class="col-md-3 mb-3">
                                    <small class="text-muted">Send Buffer</small>
                                    <div class="fw-bold" id="detailBuffer">-</div>
                                </div>
                                <div class="col-md-3 mb-3">
                                    <small class="text-muted">Congestion Window</small>
                                    <div class="fw-bold" id="detailCongestion">-</div>
                                </div>
                            </div>
                        </div>
                    </div>
                </div>
            </div>
            <div class="modal-footer">
                <button type="button" class="btn btn-secondary" data-bs-dismiss="modal">Close</button>
            </div>
        </div>
    </div>
</div>

<!-- Add Output Modal -->
<div class="modal fade" id="addOutputModal" tabindex="-1">
    <div class="modal-dialog modal-lg">
        <div class="modal-content">
            <div class="modal-header">
                <h5 class="modal-title"><i class="bi bi-plus-lg me-2"></i>Add Output</h5>
                <button type="button" class="btn-close" data-bs-dismiss="modal"></button>
            </div>
            <form id="addOutputForm">
                <input type="hidden" name="type" value="srt">
                <div class="modal-body">
                    <div class="row">
                        <div class="col-md-12 mb-3">
                            <label class="form-label">Output Name <span class="text-danger">*</span></label>
                            <input type="text" class="form-control" name="name" id="outputName" required
                                   placeholder="e.g., Main SRT Output">
                            <div id="nameValidation" class="form-text"></div>
                        </div>
                    </div>

                    <!-- UDP Input Section -->
                    <hr>
                    <h6><i class="bi bi-arrow-down-circle me-2"></i>UDP Input (from mux/transcoder)</h6>
                    <div class="alert alert-info small mb-3">
                        <i class="bi bi-info-circle me-1"></i>
                        Select a source to auto-detect the UDP output, or manually configure the address and port.
                    </div>
                    <div class="row">
                        <div class="col-md-12 mb-3">
                            <label class="form-label">Source (optional)</label>
                            <select class="form-select" id="sourceSelect">
                                <option value="">-- Manual Configuration --</option>
                            </select>
                            <small class="text-muted">Select an input, transcoder, or muxer to auto-fill UDP settings</small>
                        </div>
                    </div>
                    <div class="row">
                        <div class="col-md-6 mb-3">
                            <label class="form-label">Input Address</label>
                            <input type="text" class="form-control" name="input_address" id="inputAddress"
                                   placeholder="239.1.1.1 or leave empty for unicast">
                            <small class="text-muted">Multicast group or empty for any unicast</small>
                        </div>
                        <div class="col-md-3 mb-3">
                            <label class="form-label">Input Port <span class="text-danger">*</span></label>
                            <input type="number" class="form-control" name="input_port" id="inputPort"
                                   value="5000" min="1024" max="65535" required>
                        </div>
                        <div class="col-md-3 mb-3">
                            <label class="form-label">Interface (optional)</label>
                            <input type="text" class="form-control" name="input_interface"
                                   placeholder="e.g., 192.168.1.100">
                            <small class="text-muted">For multicast</small>
                        </div>
                    </div>

                    <!-- SRT Output Section -->
                    <hr>
                    <h6><i class="bi bi-shield-lock me-2"></i>SRT Output (One-to-Many)</h6>
                    <div class="row">
                        <div class="col-md-4 mb-3">
                            <label class="form-label">Listen Address</label>
                            <input type="text" class="form-control" name="srt_listen_address"
                                   value="0.0.0.0" placeholder="0.0.0.0">
                            <small class="text-muted">0.0.0.0 = all interfaces</small>
                        </div>
                        <div class="col-md-4 mb-3">
                            <label class="form-label">SRT Port <span class="text-danger">*</span></label>
                            <input type="number" class="form-control" name="srt_port"
                                   value="4900" min="1024" max="65535" required>
                        </div>
                        <div class="col-md-4 mb-3">
                            <label class="form-label">Max Clients</label>
                            <input type="number" class="form-control" name="srt_max_clients"
                                   value="10" min="1" max="100">
                        </div>
                    </div>
                    <div class="row">
                        <div class="col-md-4 mb-3">
                            <label class="form-label">Latency (ms)</label>
                            <input type="number" class="form-control" name="srt_latency"
                                   value="120" min="20" max="8000">
                        </div>
                        <div class="col-md-4 mb-3">
                            <label class="form-label">Stream ID (optional)</label>
                            <input type="text" class="form-control" name="srt_streamid" placeholder="">
                        </div>
                        <div class="col-md-4 mb-3">
                            <label class="form-label">Key Length</label>
                            <select class="form-select" name="srt_pbkeylen">
                                <option value="0">No Encryption</option>
                                <option value="16">AES-128</option>
                                <option value="24">AES-192</option>
                                <option value="32">AES-256</option>
                            </select>
                        </div>
                    </div>
                    <div class="row">
                        <div class="col-md-12 mb-3">
                            <label class="form-label">Encryption Passphrase (optional)</label>
                            <input type="password" class="form-control" name="srt_passphrase"
                                   placeholder="Leave empty for no encryption">
                        </div>
                    </div>

                    <!-- Service Name Preview -->
                    <div class="mt-3">
                        <small class="text-muted">Service Name: </small>
                        <code id="serviceNamePreview">-</code>
                    </div>
                </div>
                <div class="modal-footer">
                    <button type="button" class="btn btn-secondary" data-bs-dismiss="modal">Cancel</button>
                    <button type="submit" class="btn btn-primary" id="createBtn">
                        <i class="bi bi-plus-lg me-1"></i>Create Output
                    </button>
                </div>
            </form>
        </div>
    </div>
</div>

<script>
let nameValid = false;
let availableSources = [];
let clientsRefreshInterval = null;
let currentOutputId = null;

// Format duration from seconds to HH:MM:SS
function formatDuration(seconds) {
    const h = Math.floor(seconds / 3600);
    const m = Math.floor((seconds % 3600) / 60);
    const s = seconds % 60;
    return `${h.toString().padStart(2, '0')}:${m.toString().padStart(2, '0')}:${s.toString().padStart(2, '0')}`;
}

// Show clients modal
function showClientsModal(id, name) {
    currentOutputId = id;
    document.getElementById('clientsOutputId').value = id;
    document.getElementById('clientsOutputName').textContent = name;
    hideClientDetails();

    new bootstrap.Modal(document.getElementById('clientsModal')).show();
    refreshClients();
    startClientsAutoRefresh();
}

// Refresh clients list
async function refreshClients() {
    const id = document.getElementById('clientsOutputId').value;
    if (!id) return;

    try {
        const response = await fetch(`api/outputs.php?action=clients&id=${id}`);
        const data = await response.json();

        if (data.success) {
            document.getElementById('clientCount').textContent = data.client_count;
            document.getElementById('maxClients').textContent = data.max_clients;
            renderClientsTable(data.clients || []);
        } else {
            document.getElementById('clientsTableBody').innerHTML = `
                <tr><td colspan="7" class="text-center py-4 text-danger">
                    <i class="bi bi-exclamation-triangle me-2"></i>${data.error}
                </td></tr>`;
        }
    } catch (e) {
        console.error('Failed to get clients:', e);
        document.getElementById('clientsTableBody').innerHTML = `
            <tr><td colspan="7" class="text-center py-4 text-danger">
                <i class="bi bi-exclamation-triangle me-2"></i>Failed to connect to API
            </td></tr>`;
    }
}

// Render clients table
function renderClientsTable(clients) {
    const tbody = document.getElementById('clientsTableBody');

    if (clients.length === 0) {
        tbody.innerHTML = `
            <tr><td colspan="7" class="text-center py-4 text-muted">
                <i class="bi bi-people me-2"></i>No clients connected
            </td></tr>`;
        return;
    }

    let html = '';
    clients.forEach(client => {
        const duration = formatDuration(client.duration || 0);
        const rtt = (client.rtt_ms || 0).toFixed(1);
        const bw = (client.send_rate_mbps || 0).toFixed(2);
        const lost = client.packets_lost || 0;
        const retrans = client.packets_retrans || 0;

        html += `
            <tr class="client-row" onclick="showClientDetails(${client.slot})" style="cursor: pointer;">
                <td><span class="badge bg-secondary">${client.slot}</span></td>
                <td><code>${client.address}</code></td>
                <td>${duration}</td>
                <td>${rtt} ms</td>
                <td>${bw} Mbps</td>
                <td>
                    <span class="text-danger">${lost}</span> /
                    <span class="text-warning">${retrans}</span>
                </td>
                <td>
                    <button class="btn btn-outline-danger btn-sm" onclick="event.stopPropagation(); kickClient(${client.slot}, '${client.address}')" title="Kick Client">
                        <i class="bi bi-x-circle"></i>
                    </button>
                </td>
            </tr>`;
    });

    tbody.innerHTML = html;
}

// Show client details
async function showClientDetails(slot) {
    const id = document.getElementById('clientsOutputId').value;

    try {
        const response = await fetch(`api/outputs.php?action=client_info&id=${id}&slot=${slot}`);
        const data = await response.json();

        if (data.success && data.client) {
            const c = data.client;
            document.getElementById('detailClientAddr').textContent = c.address;
            document.getElementById('detailSlot').textContent = c.slot;
            document.getElementById('detailDuration').textContent = formatDuration(c.duration || 0);
            document.getElementById('detailRtt').textContent = (c.rtt_ms || 0).toFixed(1) + ' ms';
            document.getElementById('detailLatency').textContent = (c.negotiated_latency_ms || 0) + ' ms';
            document.getElementById('detailSendRate').textContent = (c.send_rate_mbps || 0).toFixed(2) + ' Mbps';
            document.getElementById('detailBandwidth').textContent = (c.bandwidth_mbps || 0).toFixed(2) + ' Mbps';
            document.getElementById('detailPacketsSent').textContent = (c.packets_sent || 0).toLocaleString();
            document.getElementById('detailSendErrors').textContent = c.send_errors || 0;
            document.getElementById('detailLost').textContent = c.packets_lost || 0;
            document.getElementById('detailRetrans').textContent = c.packets_retrans || 0;
            document.getElementById('detailDropped').textContent = c.packets_dropped || 0;
            document.getElementById('detailFlight').textContent = c.flight_size || 0;
            document.getElementById('detailBuffer').textContent = (c.send_buffer_ms || 0) + ' ms';
            document.getElementById('detailCongestion').textContent = c.congestion_window || 0;

            document.getElementById('clientDetails').classList.remove('d-none');
        }
    } catch (e) {
        console.error('Failed to get client details:', e);
    }
}

// Hide client details
function hideClientDetails() {
    document.getElementById('clientDetails').classList.add('d-none');
}

// Kick client
async function kickClient(slot, address) {
    if (!confirm(`Are you sure you want to kick client ${address}?`)) {
        return;
    }

    const id = document.getElementById('clientsOutputId').value;

    try {
        const response = await fetch(`api/outputs.php?action=kick_client&id=${id}&slot=${slot}`, {
            method: 'POST'
        });
        const data = await response.json();

        if (data.success) {
            hideClientDetails();
            refreshClients();
        } else {
            alert(data.error || 'Failed to kick client');
        }
    } catch (e) {
        alert('Error: ' + e.message);
    }
}

// Auto-refresh clients
function startClientsAutoRefresh() {
    stopClientsAutoRefresh();
    clientsRefreshInterval = setInterval(refreshClients, 5000);
}

function stopClientsAutoRefresh() {
    if (clientsRefreshInterval) {
        clearInterval(clientsRefreshInterval);
        clientsRefreshInterval = null;
    }
}

// Stop auto-refresh when modal closes
document.getElementById('clientsModal').addEventListener('hidden.bs.modal', function() {
    stopClientsAutoRefresh();
    currentOutputId = null;
});

// Output actions
function startOutput(id) {
    fetch(`api/outputs.php?action=start&id=${id}`, { method: 'POST' })
        .then(r => r.json())
        .then(data => {
            if (data.success) {
                location.reload();
            } else {
                alert(data.error || 'Failed to start output');
            }
        });
}

function stopOutput(id) {
    fetch(`api/outputs.php?action=stop&id=${id}`, { method: 'POST' })
        .then(r => r.json())
        .then(data => {
            if (data.success) {
                location.reload();
            } else {
                alert(data.error || 'Failed to stop output');
            }
        });
}

function deleteOutput(id) {
    if (confirm('Are you sure you want to delete this output? This action cannot be undone.')) {
        fetch(`api/outputs.php?action=delete&id=${id}`, { method: 'POST' })
            .then(r => r.json())
            .then(data => {
                if (data.success) {
                    location.reload();
                } else {
                    alert(data.error || 'Failed to delete output');
                }
            });
    }
}

function editOutput(id) {
    window.location.href = `outputs-edit.php?id=${id}`;
}

// Add Output Modal functions
function updateServiceNamePreview() {
    const name = document.getElementById('outputName').value;
    const id = name.toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '');
    const preview = document.getElementById('serviceNamePreview');
    preview.textContent = id ? id + '-output-srt' : '-';
}

function loadSources() {
    fetch('api/outputs.php?action=available_sources')
        .then(r => r.json())
        .then(data => {
            availableSources = data.sources || [];
            populateSourceDropdown();
        })
        .catch(err => console.error('Failed to load sources:', err));
}

function populateSourceDropdown() {
    const select = document.getElementById('sourceSelect');
    select.innerHTML = '<option value="">-- Manual Configuration --</option>';

    const groups = {
        input: { label: 'Inputs', items: [] },
        transcoder: { label: 'Transcoders', items: [] },
        muxer: { label: 'Muxers', items: [] }
    };

    availableSources.forEach(s => {
        if (groups[s.source_type]) {
            groups[s.source_type].items.push(s);
        }
    });

    Object.values(groups).forEach(group => {
        if (group.items.length > 0) {
            const optgroup = document.createElement('optgroup');
            optgroup.label = group.label;
            group.items.forEach(source => {
                const opt = document.createElement('option');
                opt.value = JSON.stringify(source);
                let label = source.display_name;
                if (source.output_port) {
                    label += ` (${source.output_address || '*'}:${source.output_port})`;
                }
                if (source.status === 'running') label += ' [Running]';
                opt.textContent = label;
                optgroup.appendChild(opt);
            });
            select.appendChild(optgroup);
        }
    });
}

document.getElementById('sourceSelect').addEventListener('change', function() {
    if (!this.value) return;
    try {
        const source = JSON.parse(this.value);
        document.getElementById('inputAddress').value = source.output_address || '';
        document.getElementById('inputPort').value = source.output_port || '5000';
    } catch (e) {}
});

let validateTimeout = null;
function validateName() {
    const nameInput = document.getElementById('outputName');
    const name = nameInput.value.trim();
    const validation = document.getElementById('nameValidation');
    const createBtn = document.getElementById('createBtn');

    if (!name) {
        validation.textContent = '';
        validation.className = 'form-text';
        nameValid = false;
        createBtn.disabled = false;
        return;
    }

    clearTimeout(validateTimeout);
    validateTimeout = setTimeout(() => {
        fetch(`api/outputs.php?action=check_name&name=${encodeURIComponent(name)}&type=srt`)
            .then(r => r.json())
            .then(data => {
                if (data.available) {
                    validation.textContent = 'Name available';
                    validation.className = 'form-text text-success';
                    nameInput.classList.remove('is-invalid');
                    nameInput.classList.add('is-valid');
                    nameValid = true;
                    createBtn.disabled = false;
                } else {
                    validation.textContent = data.config_exists ? 'Output already exists' : 'Service file exists';
                    validation.className = 'form-text text-danger';
                    nameInput.classList.remove('is-valid');
                    nameInput.classList.add('is-invalid');
                    nameValid = false;
                    createBtn.disabled = true;
                }
                updateServiceNamePreview();
            })
            .catch(() => {
                validation.textContent = 'Error checking name';
                validation.className = 'form-text text-warning';
            });
    }, 300);
}

document.getElementById('outputName').addEventListener('input', validateName);

document.getElementById('addOutputForm').addEventListener('submit', function(e) {
    e.preventDefault();

    if (!nameValid && document.getElementById('outputName').value.trim()) {
        alert('Please choose a unique name for the output');
        return;
    }

    const formData = new FormData(this);
    formData.append('action', 'create');

    fetch('api/outputs.php', { method: 'POST', body: formData })
        .then(r => r.json())
        .then(data => {
            if (data.success) {
                location.reload();
            } else {
                alert(data.error || 'Failed to create output');
            }
        })
        .catch(err => alert('Error: ' + err.message));
});

document.getElementById('addOutputModal').addEventListener('show.bs.modal', function() {
    document.getElementById('addOutputForm').reset();
    document.getElementById('nameValidation').textContent = '';
    document.getElementById('outputName').classList.remove('is-valid', 'is-invalid');
    document.getElementById('createBtn').disabled = false;
    document.getElementById('sourceSelect').value = '';
    nameValid = false;
    updateServiceNamePreview();
    loadSources();
});
</script>

<?php include __DIR__ . '/../templates/footer.php'; ?>
