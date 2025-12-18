<?php
/**
 * CariTranscoder - Inputs Management
 * Copyright (c) 2024 CariTech Solutions
 */

define('CARITRANS', true);
require_once __DIR__ . '/../includes/config.php';
require_once __DIR__ . '/../includes/auth.php';
require_once __DIR__ . '/../includes/functions.php';

auth_require_login();

$inputs = get_service_list('inputs');
$page_title = 'Inputs';
include __DIR__ . '/../templates/header.php';
?>

<div class="container-fluid py-4">
    <div class="d-flex justify-content-between align-items-center mb-4">
        <h2><i class="bi bi-download me-2"></i>Input Sources</h2>
        <button class="btn btn-primary" data-bs-toggle="modal" data-bs-target="#addInputModal">
            <i class="bi bi-plus-lg me-1"></i>Add Input
        </button>
    </div>

    <div class="row" id="inputs-grid">
        <?php if (empty($inputs)): ?>
        <div class="col-12">
            <div class="card">
                <div class="card-body text-center py-5">
                    <i class="bi bi-download text-muted" style="font-size: 3rem;"></i>
                    <h5 class="mt-3">No Input Sources</h5>
                    <p class="text-muted">Create your first input source to start receiving streams.</p>
                    <button class="btn btn-primary" data-bs-toggle="modal" data-bs-target="#addInputModal">
                        <i class="bi bi-plus-lg me-1"></i>Add Input
                    </button>
                </div>
            </div>
        </div>
        <?php else: ?>
        <?php foreach ($inputs as $input): ?>
        <div class="col-xl-4 col-md-6 mb-4">
            <div class="card service-card h-100" data-id="<?php echo htmlspecialchars($input['id']); ?>">
                <div class="card-header d-flex justify-content-between align-items-center">
                    <h6 class="mb-0"><?php echo htmlspecialchars($input['name']); ?></h6>
                    <span class="badge bg-<?php echo $input['status'] === 'running' ? 'success' : ($input['status'] === 'error' ? 'danger' : 'secondary'); ?>">
                        <?php echo ucfirst($input['status']); ?>
                    </span>
                </div>
                <div class="card-body">
                    <div class="mb-3">
                        <small class="text-muted">Type</small>
                        <div><span class="badge bg-primary"><?php echo htmlspecialchars(strtoupper($input['type'] ?? 'UDP')); ?></span></div>
                    </div>
                    <div class="mb-3">
                        <small class="text-muted">Source</small>
                        <div class="text-truncate"><?php echo htmlspecialchars($input['source'] ?? 'Not configured'); ?></div>
                    </div>
                    <div class="row">
                        <div class="col-6">
                            <small class="text-muted">Bitrate</small>
                            <div class="fw-bold"><?php echo format_bitrate($input['bitrate'] ?? 0); ?></div>
                        </div>
                        <div class="col-6">
                            <small class="text-muted">Packets</small>
                            <div class="fw-bold"><?php echo number_format($input['packets'] ?? 0); ?></div>
                        </div>
                    </div>
                </div>
                <div class="card-footer bg-transparent">
                    <div class="btn-group w-100">
                        <?php if ($input['status'] === 'running'): ?>
                        <button class="btn btn-outline-warning btn-sm" onclick="stopService('inputs', '<?php echo $input['id']; ?>')">
                            <i class="bi bi-stop-fill"></i> Stop
                        </button>
                        <?php else: ?>
                        <button class="btn btn-outline-success btn-sm" onclick="startService('inputs', '<?php echo $input['id']; ?>')">
                            <i class="bi bi-play-fill"></i> Start
                        </button>
                        <?php endif; ?>
                        <button class="btn btn-outline-secondary btn-sm" onclick="editInput('<?php echo $input['id']; ?>')">
                            <i class="bi bi-gear"></i> Edit
                        </button>
                        <button class="btn btn-outline-danger btn-sm" onclick="deleteService('inputs', '<?php echo $input['id']; ?>')">
                            <i class="bi bi-trash"></i>
                        </button>
                    </div>
                </div>
            </div>
        </div>
        <?php endforeach; ?>
        <?php endif; ?>
    </div>
</div>

<!-- Add Input Modal -->
<div class="modal fade" id="addInputModal" tabindex="-1">
    <div class="modal-dialog modal-xl">
        <div class="modal-content">
            <div class="modal-header">
                <h5 class="modal-title"><i class="bi bi-plus-lg me-2"></i>Add Input Source</h5>
                <button type="button" class="btn-close" data-bs-dismiss="modal"></button>
            </div>
            <form id="addInputForm">
                <div class="modal-body">
                    <!-- Step indicator -->
                    <ul class="nav nav-pills nav-fill mb-4" id="inputWizard">
                        <li class="nav-item">
                            <a class="nav-link active" data-step="1"><strong>1.</strong> Basic Info</a>
                        </li>
                        <li class="nav-item">
                            <a class="nav-link" data-step="2"><strong>2.</strong> Sources</a>
                        </li>
                    </ul>

                    <!-- Step 1: Basic Info -->
                    <div class="wizard-step" id="step1">
                        <h5 class="mb-3">Basic Information</h5>
                        <div class="row">
                            <div class="col-md-12 mb-3">
                                <label class="form-label">Input Name <span class="text-danger">*</span></label>
                                <input type="text" class="form-control" name="name" id="inputName" required placeholder="e.g., ESPN HD">
                                <div class="form-text" id="nameStatus"></div>
                            </div>
                        </div>
                        <div class="row">
                            <div class="col-md-6 mb-3">
                                <label class="form-label">Output Buffer</label>
                                <input type="text" class="form-control" name="buffer" id="inputBuffer" readonly>
                                <div class="form-text">Auto-generated from name</div>
                            </div>
                            <div class="col-md-6 mb-3">
                                <label class="form-label">Internal ID</label>
                                <input type="text" class="form-control" name="id" id="inputId" readonly>
                                <div class="form-text">Auto-generated from name</div>
                            </div>
                        </div>
                    </div>

                    <!-- Step 2: Sources -->
                    <div class="wizard-step" id="step2" style="display:none;">
                        <div class="d-flex justify-content-between align-items-center mb-3">
                            <h5 class="mb-0">Source Configuration</h5>
                            <button type="button" class="btn btn-sm btn-outline-primary" onclick="addSource()">
                                <i class="bi bi-plus-lg"></i> Add Failover Source
                            </button>
                        </div>
                        <p class="text-muted small">Add sources and click <strong>Configure</strong> to scan and select PIDs. Higher weight = higher priority for failover.</p>

                        <div id="sourcesContainer">
                            <!-- Sources will be added here dynamically -->
                        </div>
                    </div>
                </div>

                <div class="modal-footer">
                    <button type="button" class="btn btn-secondary" data-bs-dismiss="modal">Cancel</button>
                    <button type="button" class="btn btn-outline-secondary" id="prevBtn" onclick="prevStep()" style="display:none;">
                        <i class="bi bi-arrow-left"></i> Previous
                    </button>
                    <button type="button" class="btn btn-primary" id="nextBtn" onclick="nextStep()">
                        Next <i class="bi bi-arrow-right"></i>
                    </button>
                    <button type="submit" class="btn btn-success" id="submitBtn" style="display:none;">
                        <i class="bi bi-check-lg"></i> Create Input
                    </button>
                </div>
            </form>
        </div>
    </div>
</div>

<!-- Configure Source Modal -->
<div class="modal fade" id="configureSourceModal" tabindex="-1">
    <div class="modal-dialog modal-lg">
        <div class="modal-content">
            <div class="modal-header">
                <h5 class="modal-title"><i class="bi bi-gear me-2"></i>Configure Source</h5>
                <button type="button" class="btn-close" data-bs-dismiss="modal"></button>
            </div>
            <div class="modal-body">
                <input type="hidden" id="configSourceId">

                <!-- Source Info -->
                <div class="mb-3">
                    <div class="row">
                        <div class="col-md-3">
                            <label class="form-label">Type</label>
                            <input type="text" class="form-control" id="configSourceType" readonly>
                        </div>
                        <div class="col-md-9">
                            <label class="form-label">URL</label>
                            <input type="text" class="form-control" id="configSourceUrl" readonly>
                        </div>
                    </div>
                </div>

                <!-- Scan Button -->
                <div class="mb-4">
                    <button type="button" class="btn btn-info" id="configScanBtn" onclick="scanConfiguredSource()">
                        <i class="bi bi-search me-1"></i>Scan Source for PIDs
                    </button>
                    <span class="ms-2 text-muted small" id="configScanStatus"></span>
                </div>

                <!-- PID Selection -->
                <div class="row">
                    <div class="col-md-4 mb-3">
                        <label class="form-label">Program</label>
                        <select class="form-select" id="configProgramSelect">
                            <option value="">-- Select or enter manually --</option>
                        </select>
                        <input type="number" class="form-control mt-2" id="configProgramManual" placeholder="Or enter PID manually">
                    </div>
                    <div class="col-md-4 mb-3">
                        <label class="form-label">Video PID</label>
                        <select class="form-select" id="configVideoSelect">
                            <option value="">-- Select or enter manually --</option>
                        </select>
                        <input type="number" class="form-control mt-2" id="configVideoManual" placeholder="Or enter PID manually">
                    </div>
                    <div class="col-md-4 mb-3">
                        <label class="form-label">Audio PIDs</label>
                        <select class="form-select" id="configAudioSelect" multiple size="4">
                        </select>
                        <input type="text" class="form-control mt-2" id="configAudioManual" placeholder="Or enter PIDs: 257,258">
                        <div class="form-text">Comma-separated for multiple</div>
                    </div>
                </div>

                <!-- Scan Results -->
                <div id="configScanResults" class="mt-3" style="display:none;">
                    <div class="card bg-light">
                        <div class="card-body">
                            <h6><i class="bi bi-info-circle me-1"></i>Detected Stream Info</h6>
                            <div id="configScanResultsContent"></div>
                        </div>
                    </div>
                </div>
            </div>
            <div class="modal-footer">
                <button type="button" class="btn btn-secondary" data-bs-dismiss="modal">Cancel</button>
                <button type="button" class="btn btn-primary" onclick="saveSourceConfig()">
                    <i class="bi bi-check-lg"></i> Apply Configuration
                </button>
            </div>
        </div>
    </div>
</div>

<style>
.wizard-step {
    min-height: 300px;
}
.nav-pills .nav-link {
    border-radius: 0;
    border-bottom: 3px solid transparent;
    background: none;
    color: #6c757d;
}
.nav-pills .nav-link.active {
    background: none;
    color: #0d6efd;
    border-bottom-color: #0d6efd;
}
.nav-pills .nav-link.completed {
    color: #198754;
    border-bottom-color: #198754;
}
.source-card {
    background: #f8f9fa;
    border: 1px solid #dee2e6;
    border-radius: 0.375rem;
    padding: 1rem;
    margin-bottom: 1rem;
}
.source-card.primary {
    border-color: #0d6efd;
    background: #f0f7ff;
}
#nameStatus.valid {
    color: #198754;
}
#nameStatus.invalid {
    color: #dc3545;
}
.source-type-settings {
    background: #fff;
    border: 1px solid #e9ecef;
    border-radius: 0.25rem;
    padding: 0.75rem;
    margin-top: 0.5rem;
}
</style>

<script>
let currentStep = 1;
let nameCheckTimeout = null;
let sourcesCount = 0;

// Initialize on page load
document.addEventListener('DOMContentLoaded', function() {
    // Add first source by default
    addSource();

    // Name validation on input
    document.getElementById('inputName').addEventListener('input', function() {
        clearTimeout(nameCheckTimeout);
        nameCheckTimeout = setTimeout(() => checkNameUnique(this.value), 300);
    });

    // Form submission
    document.getElementById('addInputForm').addEventListener('submit', handleFormSubmit);
});

// Check name uniqueness
async function checkNameUnique(name) {
    const statusEl = document.getElementById('nameStatus');
    const bufferEl = document.getElementById('inputBuffer');
    const idEl = document.getElementById('inputId');

    if (!name || name.length < 2) {
        statusEl.innerHTML = '';
        bufferEl.value = '';
        idEl.value = '';
        return;
    }

    statusEl.innerHTML = '<i class="bi bi-hourglass-split"></i> Checking...';
    statusEl.className = 'form-text';

    try {
        const response = await fetch(`api/inputs.php?action=check_name&name=${encodeURIComponent(name)}`);
        const data = await response.json();

        if (data.available) {
            statusEl.innerHTML = '<i class="bi bi-check-circle"></i> Name available';
            statusEl.className = 'form-text valid';
            bufferEl.value = data.buffer_name;
            idEl.value = data.suggested_id;
        } else {
            statusEl.innerHTML = '<i class="bi bi-x-circle"></i> Name already in use';
            statusEl.className = 'form-text invalid';
            bufferEl.value = '';
            idEl.value = '';
        }
    } catch (e) {
        // If API fails (first time, no inputs exist), assume available
        statusEl.innerHTML = '<i class="bi bi-check-circle"></i> Name available';
        statusEl.className = 'form-text valid';
        // Generate ID and buffer from name
        const id = name.toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '');
        bufferEl.value = 'buffer-input-' + id;
        idEl.value = id;
    }
}

// Store source configurations (PIDs per source)
let sourceConfigs = {};

// Add a source entry
function addSource() {
    sourcesCount++;
    const isPrimary = sourcesCount === 1;
    const container = document.getElementById('sourcesContainer');

    // Initialize config for this source
    sourceConfigs[sourcesCount] = {
        program_pid: '',
        video_pid: '',
        audio_pids: []
    };

    const sourceHtml = `
        <div class="source-card ${isPrimary ? 'primary' : ''}" id="source-${sourcesCount}" data-source-id="${sourcesCount}">
            <div class="d-flex justify-content-between align-items-start mb-3">
                <span class="badge ${isPrimary ? 'bg-primary' : 'bg-secondary'}">
                    ${isPrimary ? 'Primary Source' : 'Failover Source ' + (sourcesCount - 1)}
                </span>
                <div>
                    <button type="button" class="btn btn-sm btn-outline-primary configure-btn" onclick="openConfigureModal(${sourcesCount})">
                        <i class="bi bi-gear"></i> Configure
                    </button>
                    ${!isPrimary ? `<button type="button" class="btn btn-sm btn-outline-danger ms-1" onclick="removeSource(${sourcesCount})">
                        <i class="bi bi-trash"></i>
                    </button>` : ''}
                </div>
            </div>

            <div class="row">
                <div class="col-md-3 mb-2">
                    <label class="form-label">Type</label>
                    <select class="form-select source-type" name="sources[${sourcesCount - 1}][type]" onchange="updateSourceFields(${sourcesCount})">
                        <option value="udp">UDP Multicast</option>
                        <option value="srt">SRT</option>
                        <option value="rist">RIST</option>
                        <option value="rtmp">RTMP</option>
                        <option value="hls">HLS</option>
                        <option value="file">File</option>
                    </select>
                </div>
                <div class="col-md-6 mb-2">
                    <label class="form-label source-url-label">Multicast Address:Port</label>
                    <input type="text" class="form-control source-url" name="sources[${sourcesCount - 1}][url]"
                           placeholder="239.1.1.1:5000" ${isPrimary ? 'required' : ''}>
                </div>
                <div class="col-md-3 mb-2">
                    <label class="form-label">Priority</label>
                    <input type="number" class="form-control source-weight" name="sources[${sourcesCount - 1}][weight]"
                           value="${isPrimary ? 100 : 50}" min="1" max="100">
                </div>
            </div>

            <!-- Type-specific settings (shown inline) -->
            <div class="source-type-settings" id="source-${sourcesCount}-settings" style="display:none;">
                <!-- SRT Settings -->
                <div class="srt-settings" style="display:none;">
                    <div class="row">
                        <div class="col-md-4 mb-2">
                            <label class="form-label">SRT Mode</label>
                            <select class="form-select" name="sources[${sourcesCount - 1}][srt_mode]">
                                <option value="caller">Caller</option>
                                <option value="listener">Listener</option>
                            </select>
                        </div>
                        <div class="col-md-4 mb-2">
                            <label class="form-label">Latency (ms)</label>
                            <input type="number" class="form-control" name="sources[${sourcesCount - 1}][srt_latency]" value="200">
                        </div>
                        <div class="col-md-4 mb-2">
                            <label class="form-label">Passphrase</label>
                            <input type="password" class="form-control" name="sources[${sourcesCount - 1}][srt_passphrase]" placeholder="Optional">
                        </div>
                    </div>
                </div>

                <!-- RIST Settings -->
                <div class="rist-settings" style="display:none;">
                    <div class="row">
                        <div class="col-md-4 mb-2">
                            <label class="form-label">RIST Profile</label>
                            <select class="form-select" name="sources[${sourcesCount - 1}][rist_profile]">
                                <option value="simple">Simple</option>
                                <option value="main" selected>Main</option>
                                <option value="advanced">Advanced</option>
                            </select>
                        </div>
                        <div class="col-md-4 mb-2">
                            <label class="form-label">Buffer (ms)</label>
                            <input type="number" class="form-control" name="sources[${sourcesCount - 1}][rist_buffer]" value="1000">
                        </div>
                        <div class="col-md-4 mb-2">
                            <label class="form-label">Secret (optional)</label>
                            <input type="password" class="form-control" name="sources[${sourcesCount - 1}][rist_secret]" placeholder="Encryption key">
                        </div>
                    </div>
                </div>

                <!-- File Settings -->
                <div class="file-settings" style="display:none;">
                    <div class="row">
                        <div class="col-md-6 mb-2">
                            <label class="form-label">Loop Playback</label>
                            <select class="form-select" name="sources[${sourcesCount - 1}][file_loop]">
                                <option value="1">Yes - Loop continuously</option>
                                <option value="0">No - Play once</option>
                            </select>
                        </div>
                    </div>
                </div>
            </div>

            <!-- PID Configuration Status -->
            <div class="source-config-status mt-2" id="source-${sourcesCount}-config-status" style="display:none;">
                <div class="alert alert-success mb-0 py-2">
                    <small><i class="bi bi-check-circle me-1"></i><strong>Configured:</strong> <span class="config-info"></span></small>
                </div>
            </div>
        </div>
    `;

    container.insertAdjacentHTML('beforeend', sourceHtml);
}

// Update source fields based on type
function updateSourceFields(sourceId) {
    const card = document.getElementById(`source-${sourceId}`);
    const typeSelect = card.querySelector('.source-type');
    const urlInput = card.querySelector('.source-url');
    const urlLabel = card.querySelector('.source-url-label');
    const settingsDiv = card.querySelector(`#source-${sourceId}-settings`);
    const srtSettings = card.querySelector('.srt-settings');
    const ristSettings = card.querySelector('.rist-settings');
    const fileSettings = card.querySelector('.file-settings');

    const type = typeSelect.value;

    // Hide all settings first
    srtSettings.style.display = 'none';
    ristSettings.style.display = 'none';
    fileSettings.style.display = 'none';

    // Update URL placeholder and label
    switch (type) {
        case 'udp':
            urlLabel.textContent = 'Multicast Address:Port';
            urlInput.placeholder = '239.1.1.1:5000';
            settingsDiv.style.display = 'none';
            break;
        case 'srt':
            urlLabel.textContent = 'SRT Address:Port';
            urlInput.placeholder = 'srt.server.com:9000';
            settingsDiv.style.display = 'block';
            srtSettings.style.display = 'block';
            break;
        case 'rist':
            urlLabel.textContent = 'RIST URL';
            urlInput.placeholder = 'rist://sender.example.com:5000';
            settingsDiv.style.display = 'block';
            ristSettings.style.display = 'block';
            break;
        case 'rtmp':
            urlLabel.textContent = 'RTMP URL';
            urlInput.placeholder = 'rtmp://server/app/stream';
            settingsDiv.style.display = 'none';
            break;
        case 'hls':
            urlLabel.textContent = 'HLS URL';
            urlInput.placeholder = 'https://example.com/stream.m3u8';
            settingsDiv.style.display = 'none';
            break;
        case 'file':
            urlLabel.textContent = 'File Path';
            urlInput.placeholder = '/path/to/file.ts';
            settingsDiv.style.display = 'block';
            fileSettings.style.display = 'block';
            break;
    }
}

// Remove a source entry
function removeSource(id) {
    const el = document.getElementById(`source-${id}`);
    if (el) el.remove();
}

// Wizard navigation
function nextStep() {
    if (currentStep === 1) {
        // Validate step 1
        const name = document.getElementById('inputName').value;
        const statusEl = document.getElementById('nameStatus');

        if (!name) {
            alert('Please enter an input name');
            return;
        }
        if (statusEl.classList.contains('invalid')) {
            alert('Please choose a unique name');
            return;
        }
    }

    if (currentStep < 2) {
        document.getElementById(`step${currentStep}`).style.display = 'none';
        currentStep++;
        document.getElementById(`step${currentStep}`).style.display = 'block';

        // Update nav
        document.querySelector(`[data-step="${currentStep - 1}"]`).classList.remove('active');
        document.querySelector(`[data-step="${currentStep - 1}"]`).classList.add('completed');
        document.querySelector(`[data-step="${currentStep}"]`).classList.add('active');

        // Update buttons
        document.getElementById('prevBtn').style.display = 'inline-block';
        if (currentStep === 2) {
            document.getElementById('nextBtn').style.display = 'none';
            document.getElementById('submitBtn').style.display = 'inline-block';
        }
    }
}

function prevStep() {
    if (currentStep > 1) {
        document.getElementById(`step${currentStep}`).style.display = 'none';
        currentStep--;
        document.getElementById(`step${currentStep}`).style.display = 'block';

        // Update nav
        document.querySelector(`[data-step="${currentStep + 1}"]`).classList.remove('active');
        document.querySelector(`[data-step="${currentStep}"]`).classList.remove('completed');
        document.querySelector(`[data-step="${currentStep}"]`).classList.add('active');

        // Update buttons
        if (currentStep === 1) {
            document.getElementById('prevBtn').style.display = 'none';
        }
        document.getElementById('nextBtn').style.display = 'inline-block';
        document.getElementById('submitBtn').style.display = 'none';
    }
}

// Configure Modal Functions
let configureModal = null;

function openConfigureModal(sourceId) {
    const card = document.getElementById(`source-${sourceId}`);
    if (!card) return;

    const sourceInput = card.querySelector('.source-url');
    const sourceType = card.querySelector('.source-type');

    const source = sourceInput ? sourceInput.value : '';
    const type = sourceType ? sourceType.value : 'udp';

    if (!source) {
        alert('Please enter a source URL first');
        return;
    }

    // Set modal fields
    document.getElementById('configSourceId').value = sourceId;
    document.getElementById('configSourceType').value = type.toUpperCase();
    document.getElementById('configSourceUrl').value = source;

    // Reset scan results
    document.getElementById('configScanStatus').innerHTML = '';
    document.getElementById('configScanResults').style.display = 'none';

    // Load existing config if available
    const config = sourceConfigs[sourceId] || {};
    document.getElementById('configProgramSelect').innerHTML = '<option value="">-- Scan to detect --</option>';
    document.getElementById('configVideoSelect').innerHTML = '<option value="">-- Scan to detect --</option>';
    document.getElementById('configAudioSelect').innerHTML = '';
    document.getElementById('configProgramManual').value = config.program_pid || '';
    document.getElementById('configVideoManual').value = config.video_pid || '';
    document.getElementById('configAudioManual').value = (config.audio_pids || []).join(',');

    // Show the modal
    if (!configureModal) {
        configureModal = new bootstrap.Modal(document.getElementById('configureSourceModal'));
    }
    configureModal.show();
}

async function scanConfiguredSource() {
    const sourceId = document.getElementById('configSourceId').value;
    const card = document.getElementById(`source-${sourceId}`);
    if (!card) return;

    const sourceInput = card.querySelector('.source-url');
    const sourceType = card.querySelector('.source-type');
    const source = sourceInput ? sourceInput.value : '';
    const type = sourceType ? sourceType.value : 'udp';

    const btn = document.getElementById('configScanBtn');
    const statusEl = document.getElementById('configScanStatus');

    btn.disabled = true;
    statusEl.innerHTML = '<i class="bi bi-hourglass-split"></i> Scanning source...';

    try {
        const formData = new FormData();
        formData.append('source', source);
        formData.append('type', type);

        const response = await fetch('api/inputs.php?action=scan', {
            method: 'POST',
            body: formData
        });
        const data = await response.json();

        if (data.success) {
            statusEl.innerHTML = '<i class="bi bi-check-circle text-success"></i> Scan complete';
            populateConfigPidSelects(data);
            document.getElementById('configScanResults').style.display = 'block';
        } else {
            statusEl.innerHTML = `<i class="bi bi-exclamation-triangle text-warning"></i> ${data.error || 'Scan failed'}`;
        }
    } catch (e) {
        statusEl.innerHTML = '<i class="bi bi-x-circle text-danger"></i> Scan error';
        console.error('Scan error:', e);
    }

    btn.disabled = false;
}

function populateConfigPidSelects(data) {
    const programSelect = document.getElementById('configProgramSelect');
    const videoSelect = document.getElementById('configVideoSelect');
    const audioSelect = document.getElementById('configAudioSelect');
    const resultsContent = document.getElementById('configScanResultsContent');

    // Clear existing options
    programSelect.innerHTML = '<option value="">-- Select program --</option>';
    videoSelect.innerHTML = '<option value="">-- Select video PID --</option>';
    audioSelect.innerHTML = '';

    // Populate programs
    if (data.programs && data.programs.length > 0) {
        data.programs.forEach(prog => {
            const opt = document.createElement('option');
            opt.value = prog.id || prog;
            opt.textContent = prog.name || `Program ${prog.id || prog}`;
            programSelect.appendChild(opt);
        });
    }

    // Populate video PIDs
    if (data.video_pids && data.video_pids.length > 0) {
        data.video_pids.forEach(vid => {
            const opt = document.createElement('option');
            opt.value = vid.pid;
            opt.textContent = `PID ${vid.pid} - ${vid.description || vid.codec || 'Video'}`;
            videoSelect.appendChild(opt);
        });
        // Auto-select first video
        if (data.video_pids.length === 1) {
            videoSelect.value = data.video_pids[0].pid;
        }
    }

    // Populate audio PIDs
    if (data.audio_pids && data.audio_pids.length > 0) {
        data.audio_pids.forEach(aud => {
            const opt = document.createElement('option');
            opt.value = aud.pid;
            opt.textContent = `PID ${aud.pid} - ${aud.description || aud.codec || 'Audio'} (${aud.language || 'und'})`;
            audioSelect.appendChild(opt);
        });
        // Auto-select all audio tracks
        Array.from(audioSelect.options).forEach(opt => opt.selected = true);
    }

    // Show results summary
    let html = '<div class="row">';
    html += `<div class="col-md-4"><strong>Programs:</strong> ${data.programs?.length || 0}</div>`;
    html += `<div class="col-md-4"><strong>Video:</strong> ${data.video_pids?.length || 0}</div>`;
    html += `<div class="col-md-4"><strong>Audio:</strong> ${data.audio_pids?.length || 0}</div>`;
    html += '</div>';

    if (data.video_pids && data.video_pids.length > 0) {
        html += '<div class="mt-2"><small class="text-muted">Video: ';
        html += data.video_pids.map(v => `${v.description || 'PID ' + v.pid}`).join(', ');
        html += '</small></div>';
    }

    if (data.audio_pids && data.audio_pids.length > 0) {
        html += '<div class="mt-1"><small class="text-muted">Audio: ';
        html += data.audio_pids.map(a => `${a.language || 'und'} (PID ${a.pid})`).join(', ');
        html += '</small></div>';
    }

    resultsContent.innerHTML = html;
}

function saveSourceConfig() {
    const sourceId = document.getElementById('configSourceId').value;

    // Get values from selects or manual inputs
    const programSelect = document.getElementById('configProgramSelect');
    const videoSelect = document.getElementById('configVideoSelect');
    const audioSelect = document.getElementById('configAudioSelect');

    const programPid = programSelect.value || document.getElementById('configProgramManual').value;
    const videoPid = videoSelect.value || document.getElementById('configVideoManual').value;

    // Get audio PIDs from select or manual
    let audioPids = [];
    const selectedAudio = Array.from(audioSelect.selectedOptions).map(opt => opt.value).filter(v => v);
    const manualAudio = document.getElementById('configAudioManual').value;

    if (selectedAudio.length > 0) {
        audioPids = selectedAudio;
    } else if (manualAudio) {
        audioPids = manualAudio.split(',').map(p => p.trim()).filter(p => p);
    }

    // Store config for this source
    sourceConfigs[sourceId] = {
        program_pid: programPid,
        video_pid: videoPid,
        audio_pids: audioPids
    };

    // Update the source card to show it's configured
    const statusDiv = document.getElementById(`source-${sourceId}-config-status`);
    if (statusDiv) {
        let info = [];
        if (videoPid) info.push(`Video: ${videoPid}`);
        if (audioPids.length > 0) info.push(`Audio: ${audioPids.join(',')}`);
        if (programPid) info.push(`Prog: ${programPid}`);

        statusDiv.querySelector('.config-info').textContent = info.join(' | ') || 'No PIDs selected';
        statusDiv.style.display = 'block';
    }

    // Update the configure button to show it's configured
    const card = document.getElementById(`source-${sourceId}`);
    if (card) {
        const configBtn = card.querySelector('.configure-btn');
        if (configBtn && (videoPid || audioPids.length > 0)) {
            configBtn.classList.remove('btn-outline-primary');
            configBtn.classList.add('btn-success');
            configBtn.innerHTML = '<i class="bi bi-check-circle"></i> Configured';
        }
    }

    // Close modal
    configureModal.hide();
}

// Form submission
async function handleFormSubmit(e) {
    e.preventDefault();

    const form = e.target;
    const formData = new FormData(form);

    // Validate that at least one source exists
    const sources = document.querySelectorAll('.source-url');
    let hasSource = false;
    sources.forEach(input => {
        if (input.value.trim()) hasSource = true;
    });
    if (!hasSource) {
        alert('Please enter at least one source');
        return;
    }

    // Build the data object
    const data = {
        name: formData.get('name'),
        buffer: formData.get('buffer'),
        sources: []
    };

    // Collect sources with their individual settings and PIDs
    document.querySelectorAll('.source-card').forEach((card) => {
        const sourceId = card.dataset.sourceId;
        const urlInput = card.querySelector('.source-url');
        const typeSelect = card.querySelector('.source-type');
        const weightInput = card.querySelector('.source-weight');

        if (urlInput && urlInput.value.trim()) {
            const sourceData = {
                url: urlInput.value.trim(),
                type: typeSelect ? typeSelect.value : 'udp',
                weight: parseInt(weightInput?.value || 10)
            };

            // Add type-specific settings
            const type = sourceData.type;
            if (type === 'srt') {
                const modeSelect = card.querySelector('[name*="srt_mode"]');
                const latencyInput = card.querySelector('[name*="srt_latency"]');
                const passphraseInput = card.querySelector('[name*="srt_passphrase"]');
                sourceData.srt_mode = modeSelect ? modeSelect.value : 'caller';
                sourceData.srt_latency = latencyInput ? latencyInput.value : 200;
                sourceData.srt_passphrase = passphraseInput ? passphraseInput.value : '';
            } else if (type === 'rist') {
                const profileSelect = card.querySelector('[name*="rist_profile"]');
                const bufferInput = card.querySelector('[name*="rist_buffer"]');
                const secretInput = card.querySelector('[name*="rist_secret"]');
                sourceData.rist_profile = profileSelect ? profileSelect.value : 'main';
                sourceData.rist_buffer = bufferInput ? bufferInput.value : 1000;
                sourceData.rist_secret = secretInput ? secretInput.value : '';
            } else if (type === 'file') {
                const loopSelect = card.querySelector('[name*="file_loop"]');
                sourceData.file_loop = loopSelect ? loopSelect.value : '1';
            }

            // Add PID configuration for this source
            const config = sourceConfigs[sourceId] || {};
            if (config.video_pid) sourceData.video_pid = config.video_pid;
            if (config.audio_pids && config.audio_pids.length > 0) sourceData.audio_pids = config.audio_pids;
            if (config.program_pid) sourceData.program_pid = config.program_pid;

            data.sources.push(sourceData);
        }
    });

    // Submit
    try {
        const response = await fetch('api/inputs.php?action=create', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(data)
        });

        const result = await response.json();

        if (result.success) {
            alert('Input created successfully!');
            location.reload();
        } else {
            alert('Error: ' + (result.error || 'Failed to create input'));
        }
    } catch (e) {
        console.error('Submit error:', e);
        alert('Error creating input. Please try again.');
    }
}

// Service controls
function startService(type, id) {
    fetch(`api/${type}.php?action=start&id=${id}`, { method: 'POST' })
        .then(r => r.json())
        .then(data => {
            if (data.success) location.reload();
            else alert(data.error || 'Failed to start service');
        });
}

function stopService(type, id) {
    fetch(`api/${type}.php?action=stop&id=${id}`, { method: 'POST' })
        .then(r => r.json())
        .then(data => {
            if (data.success) location.reload();
            else alert(data.error || 'Failed to stop service');
        });
}

function deleteService(type, id) {
    if (confirm('Are you sure you want to delete this input?')) {
        fetch(`api/${type}.php?action=delete&id=${id}`, { method: 'POST' })
            .then(r => {
                if (!r.ok) {
                    throw new Error(`HTTP ${r.status}`);
                }
                return r.json();
            })
            .then(data => {
                if (data.success) location.reload();
                else alert(data.error || 'Failed to delete');
            })
            .catch(e => {
                console.error('Delete error:', e);
                alert('Delete failed: ' + e.message);
            });
    }
}

function editInput(id) {
    window.location.href = `inputs-edit.php?id=${id}`;
}
</script>

<?php include __DIR__ . '/../templates/footer.php'; ?>
