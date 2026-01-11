<?php
/**
 * CariTranscoder - Muxers Management
 * TSDuck-based MPTS/SPTS multiplexing
 * Copyright (c) 2024 CariTech Solutions
 */

define('CARITRANS', true);
require_once __DIR__ . '/../includes/config.php';
require_once __DIR__ . '/../includes/auth.php';
require_once __DIR__ . '/../includes/functions.php';

auth_require_login();

$muxers = get_service_list('muxers');

// Enhance muxer data
foreach ($muxers as &$mux) {
    $config_file = CONFIG_PATH . '/muxers/' . $mux['id'] . '.conf';
    if (file_exists($config_file)) {
        $config = parse_config($config_file);
        $mux['output_bitrate'] = intval($config['output']['output_bitrate'] ?? 0);
        $mux['output_address'] = $config['output']['address'] ?? '';
        $mux['output_port'] = $config['output']['port'] ?? '';

        // Count services
        $service_count = 0;
        for ($i = 1; $i <= 20; $i++) {
            if (isset($config['services']["service.{$i}.enabled"]) &&
                $config['services']["service.{$i}.enabled"] === 'true') {
                $service_count++;
            }
        }
        $mux['service_count'] = $service_count;
    }
}

$page_title = 'Muxers';
include __DIR__ . '/../templates/header.php';
?>

<div class="container-fluid py-4">
    <div class="d-flex justify-content-between align-items-center mb-4">
        <h2><i class="bi bi-collection me-2"></i>Muxers</h2>
        <button class="btn btn-primary" onclick="showAddMuxerModal()">
            <i class="bi bi-plus-lg me-1"></i>Add Muxer
        </button>
    </div>

    <!-- Muxers Table -->
    <div class="card">
        <div class="card-body p-0">
            <div class="table-responsive">
                <table class="table table-hover mb-0" id="muxers-table">
                    <thead class="table-dark">
                        <tr>
                            <th style="width: 40px;"></th>
                            <th>Name</th>
                            <th>Services</th>
                            <th>Output</th>
                            <th>Bitrate</th>
                            <th>Status</th>
                            <th style="width: 200px;">Actions</th>
                        </tr>
                    </thead>
                    <tbody>
                        <?php if (empty($muxers)): ?>
                        <tr>
                            <td colspan="7" class="text-center py-5">
                                <i class="bi bi-collection text-muted" style="font-size: 3rem;"></i>
                                <h5 class="mt-3">No Muxers</h5>
                                <p class="text-muted">Create a muxer to combine multiple streams into MPTS.</p>
                                <button class="btn btn-primary" onclick="showAddMuxerModal()">
                                    <i class="bi bi-plus-lg me-1"></i>Add Muxer
                                </button>
                            </td>
                        </tr>
                        <?php else: ?>
                        <?php foreach ($muxers as $mux): ?>
                        <tr data-id="<?php echo htmlspecialchars($mux['id']); ?>">
                            <td>
                                <span class="status-dot status-<?php echo $mux['status'] ?? 'stopped'; ?>"></span>
                            </td>
                            <td>
                                <strong><?php echo htmlspecialchars($mux['name']); ?></strong>
                                <br><small class="text-muted"><?php echo htmlspecialchars($mux['id']); ?></small>
                            </td>
                            <td>
                                <span class="badge bg-info"><?php echo $mux['service_count'] ?? 0; ?> services</span>
                            </td>
                            <td>
                                <code><?php echo htmlspecialchars($mux['output_address'] ?? ''); ?>:<?php echo htmlspecialchars($mux['output_port'] ?? ''); ?></code>
                            </td>
                            <td>
                                <?php echo format_bitrate($mux['output_bitrate'] ?? 0); ?>
                            </td>
                            <td>
                                <span class="badge bg-<?php echo ($mux['status'] ?? 'stopped') === 'running' ? 'success' : 'secondary'; ?>">
                                    <?php echo ucfirst($mux['status'] ?? 'stopped'); ?>
                                </span>
                            </td>
                            <td>
                                <div class="btn-group btn-group-sm">
                                    <?php if (($mux['status'] ?? 'stopped') === 'running'): ?>
                                    <button class="btn btn-outline-warning" onclick="stopMuxer('<?php echo $mux['id']; ?>')" title="Stop">
                                        <i class="bi bi-stop-fill"></i>
                                    </button>
                                    <?php else: ?>
                                    <button class="btn btn-outline-success" onclick="startMuxer('<?php echo $mux['id']; ?>')" title="Start">
                                        <i class="bi bi-play-fill"></i>
                                    </button>
                                    <?php endif; ?>
                                    <button class="btn btn-outline-info" onclick="showMonitorModal('<?php echo $mux['id']; ?>')" title="Monitor">
                                        <i class="bi bi-graph-up"></i>
                                    </button>
                                    <button class="btn btn-outline-secondary" onclick="editMuxer('<?php echo $mux['id']; ?>')" title="Edit">
                                        <i class="bi bi-pencil"></i>
                                    </button>
                                    <button class="btn btn-outline-danger" onclick="deleteMuxer('<?php echo $mux['id']; ?>')" title="Delete">
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

<!-- Add/Edit Muxer Modal -->
<div class="modal fade" id="muxerModal" tabindex="-1" data-bs-backdrop="static">
    <div class="modal-dialog modal-xl">
        <div class="modal-content">
            <div class="modal-header">
                <h5 class="modal-title" id="muxerModalTitle"><i class="bi bi-plus-lg me-2"></i>Add Muxer</h5>
                <button type="button" class="btn-close" data-bs-dismiss="modal"></button>
            </div>
            <form id="muxerForm">
                <div class="modal-body">
                    <input type="hidden" id="muxerId" name="id">

                    <!-- Basic Settings -->
                    <div class="row mb-4">
                        <div class="col-md-6">
                            <label class="form-label">Name</label>
                            <input type="text" class="form-control" id="muxerName" name="name" required>
                            <small class="text-muted">e.g., mux-1, Main MPTS</small>
                        </div>
                        <div class="col-md-6">
                            <label class="form-label">Description</label>
                            <input type="text" class="form-control" id="muxerDescription" name="description">
                        </div>
                    </div>

                    <!-- Output Settings -->
                    <div class="card mb-4">
                        <div class="card-header">
                            <h6 class="mb-0"><i class="bi bi-broadcast me-2"></i>Output Settings</h6>
                        </div>
                        <div class="card-body">
                            <div class="row">
                                <div class="col-md-4">
                                    <label class="form-label">Output Bitrate (Mbps)</label>
                                    <input type="number" class="form-control" id="outputBitrate" name="output_bitrate" value="20" min="1" max="100" step="0.1" required>
                                    <small class="text-muted">Total MPTS bitrate including null packets</small>
                                </div>
                                <div class="col-md-4">
                                    <label class="form-label">Output Address</label>
                                    <input type="text" class="form-control" id="outputAddress" name="output_address" placeholder="239.1.1.1" required>
                                </div>
                                <div class="col-md-4">
                                    <label class="form-label">Output Port</label>
                                    <input type="number" class="form-control" id="outputPort" name="output_port" value="5000" min="1024" max="65535" required>
                                </div>
                            </div>
                        </div>
                    </div>

                    <!-- Services -->
                    <div class="card mb-4">
                        <div class="card-header d-flex justify-content-between align-items-center">
                            <h6 class="mb-0"><i class="bi bi-list-ul me-2"></i>Services</h6>
                            <button type="button" class="btn btn-sm btn-outline-primary" onclick="addService()">
                                <i class="bi bi-plus-lg me-1"></i>Add Service
                            </button>
                        </div>
                        <div class="card-body p-0">
                            <div id="servicesContainer">
                                <!-- Services will be added here dynamically -->
                            </div>
                            <div id="noServicesMsg" class="text-center py-4 text-muted">
                                <i class="bi bi-info-circle me-2"></i>No services added. Click "Add Service" to add streams.
                            </div>
                        </div>
                    </div>

                    <!-- Advanced Settings (collapsed by default) -->
                    <div class="accordion" id="advancedSettings">
                        <div class="accordion-item">
                            <h2 class="accordion-header">
                                <button class="accordion-button collapsed" type="button" data-bs-toggle="collapse" data-bs-target="#advancedCollapse">
                                    <i class="bi bi-gear me-2"></i>Advanced Settings
                                </button>
                            </h2>
                            <div id="advancedCollapse" class="accordion-collapse collapse" data-bs-parent="#advancedSettings">
                                <div class="accordion-body">
                                    <div class="row">
                                        <div class="col-md-3">
                                            <label class="form-label">Network ID</label>
                                            <input type="number" class="form-control" id="networkId" name="network_id" value="1">
                                        </div>
                                        <div class="col-md-3">
                                            <label class="form-label">Network Name</label>
                                            <input type="text" class="form-control" id="networkName" name="network_name" value="CariTrans">
                                        </div>
                                        <div class="col-md-3">
                                            <label class="form-label">Transport Stream ID</label>
                                            <input type="number" class="form-control" id="tsId" name="ts_id" value="1">
                                        </div>
                                        <div class="col-md-3">
                                            <label class="form-label">Original Network ID</label>
                                            <input type="number" class="form-control" id="originalNetworkId" name="original_network_id" value="1">
                                        </div>
                                    </div>
                                    <div class="row mt-3">
                                        <div class="col-md-3">
                                            <label class="form-label">PAT Interval (ms)</label>
                                            <input type="number" class="form-control" id="patInterval" name="pat_interval" value="100">
                                        </div>
                                        <div class="col-md-3">
                                            <label class="form-label">PMT Interval (ms)</label>
                                            <input type="number" class="form-control" id="pmtInterval" name="pmt_interval" value="100">
                                        </div>
                                        <div class="col-md-3">
                                            <label class="form-label">SDT Interval (ms)</label>
                                            <input type="number" class="form-control" id="sdtInterval" name="sdt_interval" value="500">
                                        </div>
                                        <div class="col-md-3">
                                            <label class="form-label">NIT Interval (ms)</label>
                                            <input type="number" class="form-control" id="nitInterval" name="nit_interval" value="10000">
                                        </div>
                                    </div>
                                </div>
                            </div>
                        </div>
                    </div>
                </div>
                <div class="modal-footer">
                    <button type="button" class="btn btn-secondary" data-bs-dismiss="modal">Cancel</button>
                    <button type="submit" class="btn btn-primary" id="saveMuxerBtn">
                        <i class="bi bi-check-lg me-1"></i>Save Muxer
                    </button>
                </div>
            </form>
        </div>
    </div>
</div>

<!-- Monitor Modal -->
<div class="modal fade" id="monitorModal" tabindex="-1">
    <div class="modal-dialog modal-xl">
        <div class="modal-content">
            <div class="modal-header">
                <h5 class="modal-title"><i class="bi bi-graph-up me-2"></i>Mux Monitor: <span id="monitorMuxName"></span></h5>
                <button type="button" class="btn-close" data-bs-dismiss="modal"></button>
            </div>
            <div class="modal-body">
                <input type="hidden" id="monitorMuxId">

                <!-- Bandwidth Bar -->
                <div class="card mb-4">
                    <div class="card-header">
                        <h6 class="mb-0">Bandwidth Usage: <span id="totalBitrateDisplay">0 Mbps</span></h6>
                    </div>
                    <div class="card-body">
                        <div class="bandwidth-bar-container mb-2">
                            <div id="bandwidthBar" class="bandwidth-bar"></div>
                        </div>
                        <div id="bandwidthLegend" class="d-flex flex-wrap gap-3 small"></div>
                    </div>
                </div>

                <!-- PID Table -->
                <div class="card">
                    <div class="card-header d-flex justify-content-between align-items-center">
                        <h6 class="mb-0">PID Breakdown</h6>
                        <button class="btn btn-sm btn-outline-secondary" onclick="refreshPidStats()">
                            <i class="bi bi-arrow-clockwise"></i> Refresh
                        </button>
                    </div>
                    <div class="card-body p-0">
                        <div class="table-responsive">
                            <table class="table table-sm mb-0" id="pidTable">
                                <thead class="table-light">
                                    <tr>
                                        <th>PID</th>
                                        <th>Type</th>
                                        <th>Description</th>
                                        <th>Bitrate</th>
                                        <th>%</th>
                                        <th style="width: 200px;">Usage</th>
                                    </tr>
                                </thead>
                                <tbody id="pidTableBody">
                                    <tr>
                                        <td colspan="6" class="text-center py-4 text-muted">
                                            <div class="spinner-border spinner-border-sm me-2"></div>
                                            Loading PID statistics...
                                        </td>
                                    </tr>
                                </tbody>
                            </table>
                        </div>
                    </div>
                </div>
            </div>
            <div class="modal-footer">
                <div class="form-check me-auto">
                    <input type="checkbox" class="form-check-input" id="autoRefreshCheck" checked>
                    <label class="form-check-label" for="autoRefreshCheck">Auto-refresh (2s)</label>
                </div>
                <button type="button" class="btn btn-secondary" data-bs-dismiss="modal">Close</button>
            </div>
        </div>
    </div>
</div>

<style>
.status-dot {
    display: inline-block;
    width: 10px;
    height: 10px;
    border-radius: 50%;
    background-color: #6c757d;
}
.status-dot.status-running {
    background-color: #198754;
    animation: pulse 2s infinite;
}
.status-dot.status-stopped {
    background-color: #6c757d;
}
@keyframes pulse {
    0%, 100% { opacity: 1; }
    50% { opacity: 0.5; }
}

.service-card {
    border: 1px solid #dee2e6;
    border-radius: 8px;
    margin-bottom: 12px;
    background: #f8f9fa;
}
.service-card .service-header {
    padding: 10px 15px;
    border-bottom: 1px solid #dee2e6;
    background: #e9ecef;
    border-radius: 7px 7px 0 0;
    display: flex;
    justify-content: space-between;
    align-items: center;
}
.service-card .service-body {
    padding: 15px;
}

.bandwidth-bar-container {
    height: 30px;
    background: #e9ecef;
    border-radius: 4px;
    overflow: hidden;
}
.bandwidth-bar {
    height: 100%;
    display: flex;
}
.bandwidth-segment {
    height: 100%;
    transition: width 0.3s ease;
}
.bandwidth-segment.video { background: #0d6efd; }
.bandwidth-segment.audio { background: #198754; }
.bandwidth-segment.psi { background: #ffc107; }
.bandwidth-segment.null { background: #dee2e6; }
.bandwidth-segment.other { background: #6c757d; }

.legend-item {
    display: flex;
    align-items: center;
    gap: 5px;
}
.legend-color {
    width: 12px;
    height: 12px;
    border-radius: 2px;
}

.pid-bar {
    height: 8px;
    background: #e9ecef;
    border-radius: 4px;
    overflow: hidden;
}
.pid-bar-fill {
    height: 100%;
    border-radius: 4px;
    transition: width 0.3s ease;
}
</style>

<script>
// Available sources cache
let availableSources = [];
let serviceTypes = {};
let serviceCount = 0;
let monitorInterval = null;
let currentMonitorId = null;

// Initialize on page load
document.addEventListener('DOMContentLoaded', function() {
    // Load available sources
    loadSources();

    // Form submit handler
    document.getElementById('muxerForm').addEventListener('submit', handleFormSubmit);

    // Monitor modal close handler
    document.getElementById('monitorModal').addEventListener('hidden.bs.modal', function() {
        if (monitorInterval) {
            clearInterval(monitorInterval);
            monitorInterval = null;
        }
        currentMonitorId = null;
    });
});

// Load available sources for service dropdown
async function loadSources() {
    try {
        const response = await fetch('api/muxers.php?action=sources');
        const data = await response.json();
        if (data.success) {
            availableSources = data.sources;
            serviceTypes = data.service_types;
        }
    } catch (e) {
        console.error('Failed to load sources:', e);
    }
}

// Show add muxer modal
async function showAddMuxerModal() {
    // Get next ID
    try {
        const response = await fetch('api/muxers.php?action=next_id');
        const data = await response.json();
        if (data.success) {
            document.getElementById('muxerName').value = data.next_name;
        }
    } catch (e) {
        console.error('Failed to get next ID:', e);
    }

    document.getElementById('muxerModalTitle').innerHTML = '<i class="bi bi-plus-lg me-2"></i>Add Muxer';
    document.getElementById('muxerId').value = '';
    document.getElementById('muxerDescription').value = '';
    document.getElementById('outputBitrate').value = '20';
    document.getElementById('outputAddress').value = '';
    document.getElementById('outputPort').value = '5000';

    // Reset services
    document.getElementById('servicesContainer').innerHTML = '';
    document.getElementById('noServicesMsg').classList.remove('d-none');
    serviceCount = 0;

    // Reset advanced settings
    document.getElementById('networkId').value = '1';
    document.getElementById('networkName').value = 'CariTrans';
    document.getElementById('tsId').value = '1';
    document.getElementById('originalNetworkId').value = '1';
    document.getElementById('patInterval').value = '100';
    document.getElementById('pmtInterval').value = '100';
    document.getElementById('sdtInterval').value = '500';
    document.getElementById('nitInterval').value = '10000';

    new bootstrap.Modal(document.getElementById('muxerModal')).show();
}

// Edit muxer
async function editMuxer(id) {
    try {
        const response = await fetch(`api/muxers.php?action=get&id=${id}`);
        const data = await response.json();

        if (!data.success) {
            alert('Failed to load muxer: ' + data.error);
            return;
        }

        const config = data.config;
        const services = data.services;

        document.getElementById('muxerModalTitle').innerHTML = '<i class="bi bi-pencil me-2"></i>Edit Muxer';
        document.getElementById('muxerId').value = id;
        document.getElementById('muxerName').value = config.muxer?.name || id;
        document.getElementById('muxerDescription').value = config.muxer?.description || '';
        document.getElementById('outputBitrate').value = (parseInt(config.output?.output_bitrate) / 1000000) || 20;
        document.getElementById('outputAddress').value = config.output?.address || '';
        document.getElementById('outputPort').value = config.output?.port || 5000;

        // Advanced settings
        document.getElementById('networkId').value = config.network?.network_id || 1;
        document.getElementById('networkName').value = config.network?.network_name || 'CariTrans';
        document.getElementById('tsId').value = config.network?.ts_id || 1;
        document.getElementById('originalNetworkId').value = config.network?.original_network_id || 1;
        document.getElementById('patInterval').value = config.tsduck?.pat_interval || 100;
        document.getElementById('pmtInterval').value = config.tsduck?.pmt_interval || 100;
        document.getElementById('sdtInterval').value = config.tsduck?.sdt_interval || 500;
        document.getElementById('nitInterval').value = config.tsduck?.nit_interval || 10000;

        // Load services
        document.getElementById('servicesContainer').innerHTML = '';
        serviceCount = 0;

        if (services && services.length > 0) {
            document.getElementById('noServicesMsg').classList.add('d-none');
            services.forEach(service => {
                if (service.enabled) {
                    addService(service);
                }
            });
        } else {
            document.getElementById('noServicesMsg').classList.remove('d-none');
        }

        new bootstrap.Modal(document.getElementById('muxerModal')).show();
    } catch (e) {
        console.error('Failed to load muxer:', e);
        alert('Failed to load muxer');
    }
}

// Add service row
function addService(data = null) {
    serviceCount++;
    const index = serviceCount;

    document.getElementById('noServicesMsg').classList.add('d-none');

    // Build source options
    let sourceOptions = '<option value="">Select source...</option>';
    availableSources.forEach(source => {
        const selected = data && data.source_id === source.id ? 'selected' : '';
        sourceOptions += `<option value="${source.id}"
            data-type="${source.type}"
            data-address="${source.address}"
            data-port="${source.port}"
            ${selected}>${source.label}</option>`;
    });

    // Build service type options
    let typeOptions = '';
    for (const [value, label] of Object.entries(serviceTypes)) {
        const selected = data && data.service_type == value ? 'selected' : '';
        typeOptions += `<option value="${value}" ${selected}>${label}</option>`;
    }

    const isPcrRef = data && data.is_pcr_reference ? 'checked' : (index === 1 ? 'checked' : '');
    const programNumber = data?.program_number || (index * 1000 + 1);
    const pmtPid = data?.pmt_pid || (256 + (index - 1));
    const videoPid = data?.video_pid || (100 + (index - 1) * 100);
    const audioPid = data?.audio_pid || (101 + (index - 1) * 100);
    const pcrPid = data?.pcr_pid || 'video';  // 'video' or 'audio'
    const streamOrder = data?.stream_order || ['video', 'audio'];  // Default: video first

    // Build stream order list HTML
    const streamOrderHtml = streamOrder.map(type => `
        <div class="stream-order-item" data-type="${type}">
            <i class="bi bi-grip-vertical me-2 stream-drag-handle"></i>
            <i class="bi bi-${type === 'video' ? 'camera-video' : 'volume-up'} me-2"></i>
            ${type === 'video' ? 'Video' : 'Audio'}
        </div>
    `).join('');

    const html = `
    <div class="service-card" id="service-${index}" data-index="${index}">
        <div class="service-header">
            <div class="d-flex align-items-center">
                <i class="bi bi-grip-vertical me-2 drag-handle" style="cursor: grab;"></i>
                <span><i class="bi bi-broadcast me-2"></i>Service ${index}</span>
            </div>
            <button type="button" class="btn btn-sm btn-outline-danger" onclick="removeService(${index})">
                <i class="bi bi-trash"></i>
            </button>
        </div>
        <div class="service-body">
            <div class="row">
                <div class="col-md-6 mb-3">
                    <label class="form-label">Source</label>
                    <select class="form-select source-select" id="source${index}" onchange="onSourceChange(${index})" required>
                        ${sourceOptions}
                    </select>
                    <input type="hidden" id="sourceType${index}">
                    <input type="hidden" id="sourceAddress${index}" value="${data?.source_address || ''}">
                    <input type="hidden" id="sourcePort${index}" value="${data?.source_port || ''}">
                </div>
                <div class="col-md-3 mb-3">
                    <label class="form-label">Program Number</label>
                    <input type="number" class="form-control" id="programNumber${index}" value="${programNumber}" min="1" max="65535" required>
                </div>
                <div class="col-md-3 mb-3">
                    <label class="form-label">PMT PID</label>
                    <input type="number" class="form-control" id="pmtPid${index}" value="${pmtPid}" min="32" max="8190">
                </div>
            </div>
            <div class="row">
                <div class="col-md-3 mb-3">
                    <label class="form-label">Video PID</label>
                    <input type="number" class="form-control" id="videoPid${index}" value="${videoPid}" min="32" max="8190" required>
                </div>
                <div class="col-md-3 mb-3">
                    <label class="form-label">Audio PID</label>
                    <input type="number" class="form-control" id="audioPid${index}" value="${audioPid}" min="32" max="8190" required>
                </div>
                <div class="col-md-3 mb-3">
                    <label class="form-label">PCR on</label>
                    <select class="form-select" id="pcrPid${index}">
                        <option value="video" ${pcrPid === 'video' ? 'selected' : ''}>Video PID</option>
                        <option value="audio" ${pcrPid === 'audio' ? 'selected' : ''}>Audio PID</option>
                    </select>
                </div>
                <div class="col-md-3 mb-3">
                    <label class="form-label">PMT Order</label>
                    <div class="stream-order-list" id="streamOrder${index}">
                        ${streamOrderHtml}
                    </div>
                </div>
            </div>
            <div class="row">
                <div class="col-md-4 mb-3">
                    <label class="form-label">Service Name</label>
                    <input type="text" class="form-control" id="serviceName${index}" value="${data?.service_name || ''}" placeholder="HD Channel 1" required>
                </div>
                <div class="col-md-4 mb-3">
                    <label class="form-label">Service Provider</label>
                    <input type="text" class="form-control" id="serviceProvider${index}" value="${data?.service_provider || 'CariTrans'}" placeholder="CariTrans">
                </div>
                <div class="col-md-4 mb-3">
                    <label class="form-label">Service Type</label>
                    <select class="form-select" id="serviceType${index}">
                        ${typeOptions}
                    </select>
                </div>
            </div>
        </div>
    </div>`;

    document.getElementById('servicesContainer').insertAdjacentHTML('beforeend', html);

    // Initialize stream order sortable for this service
    initStreamOrderSortable(index);

    // Trigger source change if data provided
    if (data && data.source_id) {
        onSourceChange(index);
    }
}

// Handle source selection change
function onSourceChange(index) {
    const select = document.getElementById(`source${index}`);
    const option = select.options[select.selectedIndex];

    if (option && option.value) {
        document.getElementById(`sourceType${index}`).value = option.dataset.type || '';
        document.getElementById(`sourceAddress${index}`).value = option.dataset.address || '';
        document.getElementById(`sourcePort${index}`).value = option.dataset.port || '';
    }
}

// Remove service
function removeService(index) {
    const element = document.getElementById(`service-${index}`);
    if (element) {
        element.remove();
    }

    // Show no services message if empty
    if (document.getElementById('servicesContainer').children.length === 0) {
        document.getElementById('noServicesMsg').classList.remove('d-none');
    }
}

// Form submit handler
async function handleFormSubmit(e) {
    e.preventDefault();

    // Collect form data
    const formData = {
        id: document.getElementById('muxerId').value,
        name: document.getElementById('muxerName').value,
        description: document.getElementById('muxerDescription').value,
        output_bitrate: Math.round(parseFloat(document.getElementById('outputBitrate').value) * 1000000),
        output_address: document.getElementById('outputAddress').value,
        output_port: parseInt(document.getElementById('outputPort').value),
        network_id: parseInt(document.getElementById('networkId').value),
        network_name: document.getElementById('networkName').value,
        ts_id: parseInt(document.getElementById('tsId').value),
        original_network_id: parseInt(document.getElementById('originalNetworkId').value),
        pat_interval: parseInt(document.getElementById('patInterval').value),
        pmt_interval: parseInt(document.getElementById('pmtInterval').value),
        sdt_interval: parseInt(document.getElementById('sdtInterval').value),
        nit_interval: parseInt(document.getElementById('nitInterval').value),
        services: []
    };

    // Collect services (in DOM order - respects drag-drop reordering)
    document.querySelectorAll('.service-card').forEach((card, orderIndex) => {
        const index = card.id.replace('service-', '');
        const sourceSelect = document.getElementById(`source${index}`);

        if (sourceSelect && sourceSelect.value) {
            const videoPid = parseInt(document.getElementById(`videoPid${index}`).value);
            const audioPid = parseInt(document.getElementById(`audioPid${index}`).value);
            const pcrOn = document.getElementById(`pcrPid${index}`).value;

            // Get stream order from sortable list
            const streamOrderList = document.getElementById(`streamOrder${index}`);
            const streamOrder = [];
            streamOrderList.querySelectorAll('.stream-order-item').forEach(item => {
                streamOrder.push(item.dataset.type);
            });

            formData.services.push({
                source_type: document.getElementById(`sourceType${index}`).value,
                source_id: sourceSelect.value,
                source_address: document.getElementById(`sourceAddress${index}`).value,
                source_port: document.getElementById(`sourcePort${index}`).value,
                program_number: parseInt(document.getElementById(`programNumber${index}`).value),
                pmt_pid: parseInt(document.getElementById(`pmtPid${index}`).value) || 0,
                video_pid: videoPid,
                audio_pid: audioPid,
                pcr_pid: pcrOn === 'video' ? videoPid : audioPid,
                stream_order: streamOrder,  // Order of streams in PMT
                service_name: document.getElementById(`serviceName${index}`).value,
                service_provider: document.getElementById(`serviceProvider${index}`).value,
                service_type: parseInt(document.getElementById(`serviceType${index}`).value),
                order: orderIndex  // Track service order
            });
        }
    });

    // Validate
    if (formData.services.length === 0) {
        alert('Please add at least one service');
        return;
    }

    // Submit
    const action = formData.id ? 'update' : 'create';
    const btn = document.getElementById('saveMuxerBtn');
    btn.disabled = true;
    btn.innerHTML = '<span class="spinner-border spinner-border-sm me-1"></span>Saving...';

    try {
        const response = await fetch(`api/muxers.php?action=${action}`, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(formData)
        });
        const result = await response.json();

        if (result.success) {
            location.reload();
        } else {
            alert('Error: ' + result.error);
        }
    } catch (e) {
        console.error('Save failed:', e);
        alert('Failed to save muxer');
    } finally {
        btn.disabled = false;
        btn.innerHTML = '<i class="bi bi-check-lg me-1"></i>Save Muxer';
    }
}

// Start muxer
async function startMuxer(id) {
    const row = document.querySelector(`tr[data-id="${id}"]`);
    const btn = row?.querySelector('.btn-outline-success');
    if (btn) {
        btn.disabled = true;
        btn.innerHTML = '<span class="spinner-border spinner-border-sm"></span>';
    }

    try {
        const response = await fetch(`api/muxers.php?action=start&id=${id}`, { method: 'POST' });
        const result = await response.json();
        if (result.success) {
            // Wait for systemd to actually start the service before reloading
            await waitForStatus(id, 'running', 5000);
            location.reload();
        } else {
            alert('Failed to start: ' + result.error);
            if (btn) {
                btn.disabled = false;
                btn.innerHTML = '<i class="bi bi-play-fill"></i>';
            }
        }
    } catch (e) {
        console.error('Start failed:', e);
        alert('Failed to start muxer');
        if (btn) {
            btn.disabled = false;
            btn.innerHTML = '<i class="bi bi-play-fill"></i>';
        }
    }
}

// Stop muxer
async function stopMuxer(id) {
    const row = document.querySelector(`tr[data-id="${id}"]`);
    const btn = row?.querySelector('.btn-outline-warning');
    if (btn) {
        btn.disabled = true;
        btn.innerHTML = '<span class="spinner-border spinner-border-sm"></span>';
    }

    try {
        const response = await fetch(`api/muxers.php?action=stop&id=${id}`, { method: 'POST' });
        const result = await response.json();
        if (result.success) {
            // Wait for systemd to actually stop the service before reloading
            await waitForStatus(id, 'stopped', 5000);
            location.reload();
        } else {
            alert('Failed to stop: ' + result.error);
            if (btn) {
                btn.disabled = false;
                btn.innerHTML = '<i class="bi bi-stop-fill"></i>';
            }
        }
    } catch (e) {
        console.error('Stop failed:', e);
        alert('Failed to stop muxer');
        if (btn) {
            btn.disabled = false;
            btn.innerHTML = '<i class="bi bi-stop-fill"></i>';
        }
    }
}

// Wait for service to reach expected status
async function waitForStatus(id, expectedStatus, timeout = 5000) {
    const startTime = Date.now();
    while (Date.now() - startTime < timeout) {
        try {
            const response = await fetch(`api/muxers.php?action=status&id=${id}`);
            const data = await response.json();
            if (data.success && data.status === expectedStatus) {
                return true;
            }
        } catch (e) {
            console.error('Status check failed:', e);
        }
        await new Promise(resolve => setTimeout(resolve, 500));
    }
    return false;  // Timeout - reload anyway
}

// Delete muxer
async function deleteMuxer(id) {
    if (!confirm('Are you sure you want to delete this muxer?')) return;

    try {
        const response = await fetch(`api/muxers.php?action=delete&id=${id}`, { method: 'POST' });
        const result = await response.json();
        if (result.success) {
            location.reload();
        } else {
            alert('Failed to delete: ' + result.error);
        }
    } catch (e) {
        console.error('Delete failed:', e);
        alert('Failed to delete muxer');
    }
}

// Show monitor modal
async function showMonitorModal(id) {
    currentMonitorId = id;
    document.getElementById('monitorMuxId').value = id;

    // Get muxer name
    const row = document.querySelector(`tr[data-id="${id}"]`);
    const name = row ? row.querySelector('strong').textContent : id;
    document.getElementById('monitorMuxName').textContent = name;

    // Show modal
    new bootstrap.Modal(document.getElementById('monitorModal')).show();

    // Start refresh
    refreshPidStats();
    startAutoRefresh();
}

// Refresh PID statistics
async function refreshPidStats() {
    const id = document.getElementById('monitorMuxId').value;
    if (!id) return;

    try {
        const response = await fetch(`api/muxers.php?action=pid_stats&id=${id}`);
        const data = await response.json();

        if (data.success) {
            renderBandwidthVisualization(data);
        } else {
            document.getElementById('pidTableBody').innerHTML = `
                <tr><td colspan="6" class="text-center py-4 text-danger">
                    <i class="bi bi-exclamation-triangle me-2"></i>${data.error}
                </td></tr>`;
        }
    } catch (e) {
        console.error('Failed to get PID stats:', e);
    }
}

// Render bandwidth visualization
function renderBandwidthVisualization(data) {
    const totalBitrate = data.total_bitrate || 0;

    // Update total display
    document.getElementById('totalBitrateDisplay').textContent = formatBitrate(totalBitrate);

    // Group PIDs by type
    const groups = {
        video: { bitrate: 0, color: '#0d6efd', label: 'Video' },
        audio: { bitrate: 0, color: '#198754', label: 'Audio' },
        psi: { bitrate: 0, color: '#ffc107', label: 'PSI/SI' },
        null: { bitrate: 0, color: '#dee2e6', label: 'Null' },
        other: { bitrate: 0, color: '#6c757d', label: 'Other' }
    };

    const pids = data.pids || [];
    pids.forEach(pid => {
        const type = pid.type;
        if (type === 'video') groups.video.bitrate += pid.bitrate;
        else if (type === 'audio') groups.audio.bitrate += pid.bitrate;
        else if (['pat', 'pmt', 'sdt', 'nit', 'eit', 'cat', 'tdt', 'rst'].includes(type)) groups.psi.bitrate += pid.bitrate;
        else if (type === 'null') groups.null.bitrate += pid.bitrate;
        else groups.other.bitrate += pid.bitrate;
    });

    // Render bandwidth bar
    let barHtml = '';
    let legendHtml = '';
    for (const [key, group] of Object.entries(groups)) {
        if (group.bitrate > 0 || key === 'null') {
            const pct = totalBitrate > 0 ? (group.bitrate / totalBitrate * 100) : 0;
            barHtml += `<div class="bandwidth-segment ${key}" style="width: ${pct}%; background: ${group.color};" title="${group.label}: ${formatBitrate(group.bitrate)} (${pct.toFixed(1)}%)"></div>`;
            legendHtml += `<div class="legend-item"><div class="legend-color" style="background: ${group.color};"></div><span>${group.label}: ${formatBitrate(group.bitrate)} (${pct.toFixed(1)}%)</span></div>`;
        }
    }
    document.getElementById('bandwidthBar').innerHTML = barHtml;
    document.getElementById('bandwidthLegend').innerHTML = legendHtml;

    // Render PID table
    let tableHtml = '';
    if (pids.length === 0) {
        tableHtml = '<tr><td colspan="6" class="text-center py-4 text-muted">No data available - muxer may not be running</td></tr>';
    } else {
        pids.forEach(pid => {
            const typeColor = getTypeColor(pid.type);
            tableHtml += `
            <tr>
                <td><code>${pid.pid_hex}</code> <small class="text-muted">(${pid.pid})</small></td>
                <td><span class="badge" style="background: ${typeColor};">${pid.type.toUpperCase()}</span></td>
                <td>${pid.description || '-'}</td>
                <td>${formatBitrate(pid.bitrate)}</td>
                <td>${pid.percentage.toFixed(1)}%</td>
                <td>
                    <div class="pid-bar">
                        <div class="pid-bar-fill" style="width: ${pid.percentage}%; background: ${typeColor};"></div>
                    </div>
                </td>
            </tr>`;
        });
    }
    document.getElementById('pidTableBody').innerHTML = tableHtml;
}

function getTypeColor(type) {
    const colors = {
        video: '#0d6efd',
        audio: '#198754',
        pmt: '#ffc107',
        pat: '#ffc107',
        sdt: '#ffc107',
        nit: '#ffc107',
        eit: '#ffc107',
        cat: '#ffc107',
        tdt: '#ffc107',
        rst: '#ffc107',
        pcr: '#17a2b8',
        subtitle: '#6f42c1',
        null: '#dee2e6',
        other: '#6c757d'
    };
    return colors[type] || '#6c757d';
}

function formatBitrate(bps) {
    if (!bps || bps === 0) return '0 bps';
    if (bps >= 1000000) return (bps / 1000000).toFixed(2) + ' Mbps';
    if (bps >= 1000) return (bps / 1000).toFixed(1) + ' Kbps';
    return bps + ' bps';
}

// Auto-refresh handling
function startAutoRefresh() {
    const checkbox = document.getElementById('autoRefreshCheck');
    if (checkbox.checked && !monitorInterval) {
        monitorInterval = setInterval(refreshPidStats, 2000);
    }

    checkbox.addEventListener('change', function() {
        if (this.checked) {
            monitorInterval = setInterval(refreshPidStats, 2000);
        } else if (monitorInterval) {
            clearInterval(monitorInterval);
            monitorInterval = null;
        }
    });
}

// Initialize SortableJS when modal opens
let servicesSortable = null;

function initServicesSortable() {
    const container = document.getElementById('servicesContainer');
    if (container && typeof Sortable !== 'undefined') {
        if (servicesSortable) {
            servicesSortable.destroy();
        }
        servicesSortable = new Sortable(container, {
            animation: 150,
            handle: '.drag-handle',
            ghostClass: 'sortable-ghost',
            onEnd: function() {
                // Update service numbers after reorder
                updateServiceNumbers();
            }
        });
    }
}

function updateServiceNumbers() {
    document.querySelectorAll('.service-card').forEach((card, idx) => {
        const header = card.querySelector('.service-header span:last-child');
        if (header) {
            header.innerHTML = `<i class="bi bi-broadcast me-2"></i>Service ${idx + 1}`;
        }
    });
}

// Initialize stream order sortable for a specific service
function initStreamOrderSortable(index) {
    const container = document.getElementById(`streamOrder${index}`);
    if (container && typeof Sortable !== 'undefined') {
        new Sortable(container, {
            animation: 150,
            handle: '.stream-drag-handle',
            ghostClass: 'stream-sortable-ghost'
        });
    }
}

// Re-init all stream order sortables
function initAllStreamOrderSortables() {
    document.querySelectorAll('.stream-order-list').forEach(list => {
        const index = list.id.replace('streamOrder', '');
        initStreamOrderSortable(index);
    });
}

// Re-init sortable when modal shown
document.getElementById('muxerModal')?.addEventListener('shown.bs.modal', function() {
    setTimeout(() => {
        initServicesSortable();
        initAllStreamOrderSortables();
    }, 100);
});
</script>

<!-- SortableJS for drag-drop service ordering -->
<script src="https://cdn.jsdelivr.net/npm/sortablejs@1.15.0/Sortable.min.js"></script>

<style>
.sortable-ghost {
    opacity: 0.4;
    background: #e9ecef;
}
.drag-handle:hover {
    color: #0d6efd;
}
.stream-order-list {
    border: 1px solid #dee2e6;
    border-radius: 4px;
    padding: 4px;
    background: #f8f9fa;
}
.stream-order-item {
    display: flex;
    align-items: center;
    padding: 4px 8px;
    margin: 2px 0;
    background: white;
    border: 1px solid #dee2e6;
    border-radius: 3px;
    font-size: 0.85rem;
    cursor: default;
}
.stream-drag-handle {
    cursor: grab;
    color: #6c757d;
}
.stream-drag-handle:hover {
    color: #0d6efd;
}
.stream-sortable-ghost {
    opacity: 0.4;
    background: #cfe2ff;
}
</style>

<?php include __DIR__ . '/../templates/footer.php'; ?>
