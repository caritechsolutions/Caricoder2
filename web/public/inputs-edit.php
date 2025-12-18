<?php
/**
 * CariTranscoder - Edit Input
 * Copyright (c) 2024 CariTech Solutions
 */

define('CARITRANS', true);
require_once __DIR__ . '/../includes/config.php';
require_once __DIR__ . '/../includes/auth.php';
require_once __DIR__ . '/../includes/functions.php';

auth_require_login();

$id = $_GET['id'] ?? '';
if (empty($id)) {
    header('Location: inputs.php');
    exit;
}

// Sanitize ID
$id = preg_replace('/[^a-zA-Z0-9_-]/', '', $id);
$config_file = CONFIG_DIR . '/inputs/' . $id . '.conf';

if (!file_exists($config_file)) {
    header('Location: inputs.php?error=notfound');
    exit;
}

$config = parse_config($config_file);
$input_name = $config['general']['name'] ?? $id;
$input_type = $config['general']['type'] ?? 'udp';
$input_buffer = $config['general']['buffer'] ?? '';

// Parse sources with per-source PIDs
$sources = [];
if (isset($config['sources'])) {
    foreach ($config['sources'] as $key => $value) {
        if (strpos($key, 'source_') === 0) {
            $parts = explode('|', $value);
            $source = [
                'type' => $parts[0] ?? 'udp',
                'url' => $parts[1] ?? '',
                'weight' => $parts[2] ?? 100,
                'video_pid' => '',
                'audio_pids' => [],
                'program_pid' => ''
            ];
            // Parse extra settings including PIDs
            if (isset($parts[3])) {
                $extras = explode(',', $parts[3]);
                foreach ($extras as $extra) {
                    $kv = explode('=', $extra, 2);
                    if (count($kv) == 2) {
                        if ($kv[0] === 'audio_pids') {
                            $source['audio_pids'] = explode(';', $kv[1]);
                        } else {
                            $source[$kv[0]] = $kv[1];
                        }
                    }
                }
            }
            $sources[] = $source;
        }
    }
}

// Global PIDs for backwards compatibility (from first source or pids section)
$video_pid = $config['pids']['video'] ?? '';
$audio_pids = $config['pids']['audio'] ?? '';
$program_pid = $config['pids']['program'] ?? '';

// Apply global PIDs to first source if it doesn't have its own
if (!empty($sources) && empty($sources[0]['video_pid'])) {
    $sources[0]['video_pid'] = $video_pid;
}
if (!empty($sources) && empty($sources[0]['audio_pids']) && $audio_pids) {
    $sources[0]['audio_pids'] = explode(',', $audio_pids);
}
if (!empty($sources) && empty($sources[0]['program_pid'])) {
    $sources[0]['program_pid'] = $program_pid;
}

$page_title = 'Edit Input: ' . $input_name;
include __DIR__ . '/../templates/header.php';
?>

<div class="container-fluid py-4">
    <div class="d-flex justify-content-between align-items-center mb-4">
        <h2><i class="bi bi-pencil me-2"></i>Edit Input: <?php echo htmlspecialchars($input_name); ?></h2>
        <a href="inputs.php" class="btn btn-secondary">
            <i class="bi bi-arrow-left me-1"></i>Back to Inputs
        </a>
    </div>

    <form id="editInputForm">
        <input type="hidden" name="id" value="<?php echo htmlspecialchars($id); ?>">

        <div class="card mb-4">
            <div class="card-header">
                <h5 class="mb-0">Basic Information</h5>
            </div>
            <div class="card-body">
                <div class="row">
                    <div class="col-md-6 mb-3">
                        <label class="form-label">Input Name</label>
                        <input type="text" class="form-control" name="name" value="<?php echo htmlspecialchars($input_name); ?>" required>
                    </div>
                    <div class="col-md-6 mb-3">
                        <label class="form-label">Output Buffer</label>
                        <input type="text" class="form-control" name="buffer" value="<?php echo htmlspecialchars($input_buffer); ?>">
                    </div>
                </div>
            </div>
        </div>

        <div class="card mb-4">
            <div class="card-header d-flex justify-content-between align-items-center">
                <h5 class="mb-0">Sources</h5>
                <button type="button" class="btn btn-sm btn-outline-primary" onclick="addSource()">
                    <i class="bi bi-plus-lg"></i> Add Failover Source
                </button>
            </div>
            <div class="card-body">
                <p class="text-muted small">Configure each source and click <strong>Configure</strong> to scan and select PIDs.</p>
                <div id="sourcesContainer">
                    <!-- Sources will be populated by JavaScript -->
                </div>
            </div>
        </div>

        <div class="d-flex gap-2">
            <button type="submit" class="btn btn-primary">
                <i class="bi bi-check-lg me-1"></i>Save Changes
            </button>
            <a href="inputs.php" class="btn btn-secondary">Cancel</a>
            <button type="button" class="btn btn-danger ms-auto" onclick="deleteInput()">
                <i class="bi bi-trash me-1"></i>Delete Input
            </button>
        </div>
    </form>
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
.source-type-settings {
    background: #fff;
    border: 1px solid #e9ecef;
    border-radius: 0.25rem;
    padding: 0.75rem;
    margin-top: 0.5rem;
}
</style>

<script>
const inputId = '<?php echo htmlspecialchars($id); ?>';

// Initial sources from PHP
const initialSources = <?php echo json_encode($sources); ?>;

// Source configurations (PIDs per source)
let sourceConfigs = {};
let sourcesCount = 0;
let configureModal = null;

document.addEventListener('DOMContentLoaded', function() {
    // Load existing sources
    if (initialSources.length > 0) {
        initialSources.forEach((source, idx) => {
            addSource(source);
        });
    } else {
        addSource();
    }

    document.getElementById('editInputForm').addEventListener('submit', handleSubmit);
});

function addSource(existingSource = null) {
    sourcesCount++;
    const isPrimary = document.querySelectorAll('.source-card').length === 0;
    const container = document.getElementById('sourcesContainer');

    // Initialize config for this source
    sourceConfigs[sourcesCount] = {
        program_pid: existingSource?.program_pid || '',
        video_pid: existingSource?.video_pid || '',
        audio_pids: existingSource?.audio_pids || []
    };

    const type = existingSource?.type || 'udp';
    const url = existingSource?.url || '';
    const weight = existingSource?.weight || (isPrimary ? 100 : 50);

    // Check if source has PIDs configured
    const hasConfig = sourceConfigs[sourcesCount].video_pid ||
                      (sourceConfigs[sourcesCount].audio_pids && sourceConfigs[sourcesCount].audio_pids.length > 0);

    const sourceHtml = `
        <div class="source-card ${isPrimary ? 'primary' : ''}" id="source-${sourcesCount}" data-source-id="${sourcesCount}">
            <div class="d-flex justify-content-between align-items-start mb-3">
                <span class="badge ${isPrimary ? 'bg-primary' : 'bg-secondary'}">
                    ${isPrimary ? 'Primary Source' : 'Failover Source ' + (sourcesCount - 1)}
                </span>
                <div>
                    <button type="button" class="btn btn-sm ${hasConfig ? 'btn-success' : 'btn-outline-primary'} configure-btn" onclick="openConfigureModal(${sourcesCount})">
                        <i class="bi ${hasConfig ? 'bi-check-circle' : 'bi-gear'}"></i> ${hasConfig ? 'Configured' : 'Configure'}
                    </button>
                    ${!isPrimary ? `<button type="button" class="btn btn-sm btn-outline-danger ms-1" onclick="removeSource(${sourcesCount})">
                        <i class="bi bi-trash"></i>
                    </button>` : ''}
                </div>
            </div>

            <div class="row">
                <div class="col-md-3 mb-2">
                    <label class="form-label">Type</label>
                    <select class="form-select source-type" onchange="updateSourceFields(${sourcesCount})">
                        <option value="udp" ${type === 'udp' ? 'selected' : ''}>UDP Multicast</option>
                        <option value="srt" ${type === 'srt' ? 'selected' : ''}>SRT</option>
                        <option value="rtmp" ${type === 'rtmp' ? 'selected' : ''}>RTMP</option>
                        <option value="hls" ${type === 'hls' ? 'selected' : ''}>HLS</option>
                        <option value="file" ${type === 'file' ? 'selected' : ''}>File</option>
                    </select>
                </div>
                <div class="col-md-6 mb-2">
                    <label class="form-label source-url-label">Source URL</label>
                    <input type="text" class="form-control source-url" value="${escapeHtml(url)}" ${isPrimary ? 'required' : ''}>
                </div>
                <div class="col-md-3 mb-2">
                    <label class="form-label">Priority</label>
                    <input type="number" class="form-control source-weight" value="${weight}" min="1" max="100">
                </div>
            </div>

            <!-- Type-specific settings -->
            <div class="source-type-settings" id="source-${sourcesCount}-settings" style="display:${type === 'srt' || type === 'file' ? 'block' : 'none'};">
                <!-- SRT Settings -->
                <div class="srt-settings" style="display:${type === 'srt' ? 'block' : 'none'};">
                    <div class="row">
                        <div class="col-md-4 mb-2">
                            <label class="form-label">SRT Mode</label>
                            <select class="form-select srt-mode">
                                <option value="caller" ${(existingSource?.mode || 'caller') === 'caller' ? 'selected' : ''}>Caller</option>
                                <option value="listener" ${existingSource?.mode === 'listener' ? 'selected' : ''}>Listener</option>
                            </select>
                        </div>
                        <div class="col-md-4 mb-2">
                            <label class="form-label">Latency (ms)</label>
                            <input type="number" class="form-control srt-latency" value="${existingSource?.latency || 200}">
                        </div>
                        <div class="col-md-4 mb-2">
                            <label class="form-label">Passphrase</label>
                            <input type="password" class="form-control srt-passphrase" value="${existingSource?.passphrase || ''}" placeholder="Optional">
                        </div>
                    </div>
                </div>

                <!-- File Settings -->
                <div class="file-settings" style="display:${type === 'file' ? 'block' : 'none'};">
                    <div class="row">
                        <div class="col-md-6 mb-2">
                            <label class="form-label">Loop Playback</label>
                            <select class="form-select file-loop">
                                <option value="1" ${(existingSource?.loop || '1') === '1' ? 'selected' : ''}>Yes - Loop continuously</option>
                                <option value="0" ${existingSource?.loop === '0' ? 'selected' : ''}>No - Play once</option>
                            </select>
                        </div>
                    </div>
                </div>
            </div>

            <!-- PID Configuration Status -->
            <div class="source-config-status mt-2" id="source-${sourcesCount}-config-status" style="display:${hasConfig ? 'block' : 'none'};">
                <div class="alert alert-success mb-0 py-2">
                    <small><i class="bi bi-check-circle me-1"></i><strong>Configured:</strong> <span class="config-info">${getConfigInfo(sourcesCount)}</span></small>
                </div>
            </div>
        </div>
    `;

    container.insertAdjacentHTML('beforeend', sourceHtml);
}

function getConfigInfo(sourceId) {
    const config = sourceConfigs[sourceId] || {};
    let info = [];
    if (config.video_pid) info.push(`Video: ${config.video_pid}`);
    if (config.audio_pids && config.audio_pids.length > 0) info.push(`Audio: ${config.audio_pids.join(',')}`);
    if (config.program_pid) info.push(`Prog: ${config.program_pid}`);
    return info.join(' | ') || 'No PIDs selected';
}

function escapeHtml(str) {
    if (!str) return '';
    return str.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
}

function updateSourceFields(sourceId) {
    const card = document.getElementById(`source-${sourceId}`);
    const typeSelect = card.querySelector('.source-type');
    const urlInput = card.querySelector('.source-url');
    const urlLabel = card.querySelector('.source-url-label');
    const settingsDiv = card.querySelector(`#source-${sourceId}-settings`);
    const srtSettings = card.querySelector('.srt-settings');
    const fileSettings = card.querySelector('.file-settings');

    const type = typeSelect.value;

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
            fileSettings.style.display = 'none';
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
            srtSettings.style.display = 'none';
            fileSettings.style.display = 'block';
            break;
    }
}

function removeSource(id) {
    const el = document.getElementById(`source-${id}`);
    if (el) {
        el.remove();
        delete sourceConfigs[id];
    }
}

// Configure Modal Functions
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

    // Load existing config
    const config = sourceConfigs[sourceId] || {};
    document.getElementById('configProgramSelect').innerHTML = '<option value="">-- Scan to detect --</option>';
    document.getElementById('configVideoSelect').innerHTML = '<option value="">-- Scan to detect --</option>';
    document.getElementById('configAudioSelect').innerHTML = '';
    document.getElementById('configProgramManual').value = config.program_pid || '';
    document.getElementById('configVideoManual').value = config.video_pid || '';
    document.getElementById('configAudioManual').value = (config.audio_pids || []).join(',');

    // Show modal
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

    programSelect.innerHTML = '<option value="">-- Select program --</option>';
    videoSelect.innerHTML = '<option value="">-- Select video PID --</option>';
    audioSelect.innerHTML = '';

    if (data.programs && data.programs.length > 0) {
        data.programs.forEach(prog => {
            const opt = document.createElement('option');
            opt.value = prog.id || prog;
            opt.textContent = prog.name || `Program ${prog.id || prog}`;
            programSelect.appendChild(opt);
        });
    }

    if (data.video_pids && data.video_pids.length > 0) {
        data.video_pids.forEach(vid => {
            const opt = document.createElement('option');
            opt.value = vid.pid;
            opt.textContent = `PID ${vid.pid} - ${vid.description || vid.codec || 'Video'}`;
            videoSelect.appendChild(opt);
        });
        if (data.video_pids.length === 1) {
            videoSelect.value = data.video_pids[0].pid;
        }
    }

    if (data.audio_pids && data.audio_pids.length > 0) {
        data.audio_pids.forEach(aud => {
            const opt = document.createElement('option');
            opt.value = aud.pid;
            opt.textContent = `PID ${aud.pid} - ${aud.description || aud.codec || 'Audio'} (${aud.language || 'und'})`;
            audioSelect.appendChild(opt);
        });
        Array.from(audioSelect.options).forEach(opt => opt.selected = true);
    }

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

    const programSelect = document.getElementById('configProgramSelect');
    const videoSelect = document.getElementById('configVideoSelect');
    const audioSelect = document.getElementById('configAudioSelect');

    const programPid = programSelect.value || document.getElementById('configProgramManual').value;
    const videoPid = videoSelect.value || document.getElementById('configVideoManual').value;

    let audioPids = [];
    const selectedAudio = Array.from(audioSelect.selectedOptions).map(opt => opt.value).filter(v => v);
    const manualAudio = document.getElementById('configAudioManual').value;

    if (selectedAudio.length > 0) {
        audioPids = selectedAudio;
    } else if (manualAudio) {
        audioPids = manualAudio.split(',').map(p => p.trim()).filter(p => p);
    }

    sourceConfigs[sourceId] = {
        program_pid: programPid,
        video_pid: videoPid,
        audio_pids: audioPids
    };

    // Update source card UI
    const statusDiv = document.getElementById(`source-${sourceId}-config-status`);
    if (statusDiv) {
        statusDiv.querySelector('.config-info').textContent = getConfigInfo(sourceId);
        statusDiv.style.display = 'block';
    }

    const card = document.getElementById(`source-${sourceId}`);
    if (card) {
        const configBtn = card.querySelector('.configure-btn');
        if (configBtn && (videoPid || audioPids.length > 0)) {
            configBtn.classList.remove('btn-outline-primary');
            configBtn.classList.add('btn-success');
            configBtn.innerHTML = '<i class="bi bi-check-circle"></i> Configured';
        }
    }

    configureModal.hide();
}

async function handleSubmit(e) {
    e.preventDefault();

    const form = e.target;
    const formData = new FormData(form);

    const data = {
        id: inputId,
        name: formData.get('name'),
        buffer: formData.get('buffer'),
        sources: []
    };

    // Collect sources with their PIDs
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
                const modeSelect = card.querySelector('.srt-mode');
                const latencyInput = card.querySelector('.srt-latency');
                const passphraseInput = card.querySelector('.srt-passphrase');
                sourceData.srt_mode = modeSelect ? modeSelect.value : 'caller';
                sourceData.srt_latency = latencyInput ? latencyInput.value : 200;
                sourceData.srt_passphrase = passphraseInput ? passphraseInput.value : '';
            } else if (type === 'file') {
                const loopSelect = card.querySelector('.file-loop');
                sourceData.file_loop = loopSelect ? loopSelect.value : '1';
            }

            // Add PID configuration
            const config = sourceConfigs[sourceId] || {};
            if (config.video_pid) sourceData.video_pid = config.video_pid;
            if (config.audio_pids && config.audio_pids.length > 0) sourceData.audio_pids = config.audio_pids;
            if (config.program_pid) sourceData.program_pid = config.program_pid;

            data.sources.push(sourceData);
        }
    });

    try {
        const response = await fetch(`api/inputs.php?action=update&id=${inputId}`, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(data)
        });

        const result = await response.json();

        if (result.success) {
            alert('Input updated successfully!');
            window.location.href = 'inputs.php';
        } else {
            alert('Error: ' + (result.error || 'Failed to update input'));
        }
    } catch (e) {
        console.error('Submit error:', e);
        alert('Error updating input');
    }
}

function deleteInput() {
    if (confirm('Are you sure you want to delete this input?')) {
        fetch(`api/inputs.php?action=delete&id=${inputId}`, { method: 'POST' })
            .then(r => r.json())
            .then(data => {
                if (data.success) {
                    window.location.href = 'inputs.php';
                } else {
                    alert(data.error || 'Failed to delete');
                }
            });
    }
}
</script>

<?php include __DIR__ . '/../templates/footer.php'; ?>
