<?php
/**
 * CariTranscoder - Transcoders Management
 * Copyright (c) 2024 CariTech Solutions
 */

define('CARITRANS', true);
require_once __DIR__ . '/../includes/config.php';
require_once __DIR__ . '/../includes/auth.php';
require_once __DIR__ . '/../includes/functions.php';

auth_require_login();

$transcoders = get_service_list('transcoders');

// Load full config for each transcoder
foreach ($transcoders as &$transcoder) {
    $config_file = CONFIG_PATH . '/transcoders/' . $transcoder['id'] . '.conf';
    if (file_exists($config_file)) {
        $transcoder['config'] = parse_config($config_file);
    }
}
unset($transcoder);

$page_title = 'Transcoders';
include __DIR__ . '/../templates/header.php';

// Helper function to get codec display name
function getCodecDisplay($codec) {
    $codecs = [
        'h264' => 'H.264',
        'h265' => 'H.265',
        'hevc' => 'HEVC',
        'mpeg2' => 'MPEG-2',
        'aac' => 'AAC',
        'ac3' => 'AC3',
        'mp2' => 'MP2',
        'opus' => 'Opus'
    ];
    return $codecs[strtolower($codec)] ?? strtoupper($codec);
}

// Helper function to get resolution from scaling config
function getResolution($config) {
    if (config_bool($config['scaling']['enabled'] ?? false)) {
        $width = $config['scaling']['width'] ?? '?';
        $height = $config['scaling']['height'] ?? '?';
        return "{$width}x{$height}";
    }
    return 'Passthrough';
}
?>

<style>
.status-dot {
    display: inline-block;
    width: 10px;
    height: 10px;
    border-radius: 50%;
    margin-right: 4px;
}
.status-running { background-color: #28a745; box-shadow: 0 0 4px #28a745; }
.status-stopped { background-color: #6c757d; }
.status-error { background-color: #dc3545; box-shadow: 0 0 4px #dc3545; }

.row-odd { background-color: rgba(0,0,0,0.02); }
.row-even { background-color: transparent; }

.bitrate-cell {
    font-family: 'Monaco', 'Consolas', monospace;
    font-size: 0.85rem;
}
.bitrate-cell.offline {
    opacity: 0.5;
}
.bitrate-video { color: #0d6efd; font-weight: 500; }
.bitrate-audio { color: #6c757d; }

.codec-badge {
    font-size: 0.75rem;
    padding: 0.2rem 0.4rem;
}

.transcoder-row:hover {
    background-color: rgba(13, 110, 253, 0.05) !important;
}
</style>

<div class="container-fluid py-4">
    <div class="d-flex justify-content-between align-items-center mb-3">
        <h2 class="mb-0"><i class="bi bi-arrow-repeat me-2"></i>Transcoders</h2>
        <a href="transcoders-edit.php" class="btn btn-primary">
            <i class="bi bi-plus-lg me-1"></i>Add Transcoder
        </a>
    </div>

    <!-- Search and Filter Bar -->
    <div class="card mb-3">
        <div class="card-body py-2">
            <div class="row align-items-center">
                <div class="col-md-4">
                    <div class="input-group">
                        <span class="input-group-text bg-transparent border-end-0"><i class="bi bi-search"></i></span>
                        <input type="text" class="form-control border-start-0" id="searchInput" placeholder="Search transcoders..." onkeyup="filterTranscoders()">
                    </div>
                </div>
                <div class="col-md-3">
                    <select class="form-select" id="codecFilter" onchange="filterTranscoders()">
                        <option value="">All Codecs</option>
                        <option value="h264">H.264</option>
                        <option value="h265">H.265</option>
                        <option value="mpeg2">MPEG-2</option>
                    </select>
                </div>
                <div class="col-md-3">
                    <select class="form-select" id="statusFilter" onchange="filterTranscoders()">
                        <option value="">All Status</option>
                        <option value="running">Running</option>
                        <option value="stopped">Stopped</option>
                        <option value="error">Error</option>
                    </select>
                </div>
                <div class="col-md-2 text-end">
                    <span class="text-muted" id="transcoderCount"><?php echo count($transcoders); ?> transcoders</span>
                </div>
            </div>
        </div>
    </div>

    <!-- Transcoders Table -->
    <div class="card">
        <div class="table-responsive">
            <table class="table align-middle mb-0" id="transcodersTable">
                <thead class="table-light">
                    <tr>
                        <th style="width: 40px;"></th>
                        <th>Name</th>
                        <th>Video</th>
                        <th>Audio</th>
                        <th>Input</th>
                        <th>Output</th>
                        <th style="width: 140px;">Output Bitrate</th>
                        <th style="width: 150px;">Actions</th>
                    </tr>
                </thead>
                <tbody>
                    <?php if (empty($transcoders)): ?>
                    <tr id="emptyRow">
                        <td colspan="8" class="text-center py-5">
                            <i class="bi bi-arrow-repeat text-muted" style="font-size: 2rem;"></i>
                            <p class="mt-2 mb-0 text-muted">No transcoders configured</p>
                            <a href="transcoders-edit.php" class="btn btn-primary btn-sm mt-2">
                                <i class="bi bi-plus-lg me-1"></i>Add Transcoder
                            </a>
                        </td>
                    </tr>
                    <?php else: ?>
                    <?php $rowNum = 0; foreach ($transcoders as $transcoder): $rowNum++;
                        $config = $transcoder['config'] ?? [];
                        $videoCodec = $config['video']['codec'] ?? 'h264';
                        $audioCodec = $config['audio']['codec'] ?? 'aac';
                        $videoBitrate = $config['video']['bitrate'] ?? 0;
                        $audioBitrate = $config['audio']['bitrate'] ?? 0;
                        $inputAddr = ($config['input']['address'] ?? '') . ':' . ($config['input']['port'] ?? '');
                        $outputAddr = ($config['output']['address'] ?? '') . ':' . ($config['output']['port'] ?? '');
                        $resolution = getResolution($config);
                    ?>
                    <tr class="transcoder-row <?php echo ($rowNum % 2 == 0) ? 'row-even' : 'row-odd'; ?>"
                        data-id="<?php echo htmlspecialchars($transcoder['id']); ?>"
                        data-name="<?php echo htmlspecialchars(strtolower($transcoder['name'])); ?>"
                        data-codec="<?php echo htmlspecialchars(strtolower($videoCodec)); ?>"
                        data-status="<?php echo htmlspecialchars($transcoder['status']); ?>">
                        <td>
                            <span class="status-dot status-<?php echo $transcoder['status']; ?>"
                                  id="status-<?php echo $transcoder['id']; ?>"
                                  title="<?php echo ucfirst($transcoder['status']); ?>"></span>
                        </td>
                        <td>
                            <strong><?php echo htmlspecialchars($transcoder['name']); ?></strong>
                        </td>
                        <td>
                            <span class="badge bg-info codec-badge"><?php echo getCodecDisplay($videoCodec); ?></span>
                            <small class="text-muted ms-1"><?php echo $resolution; ?></small>
                            <br>
                            <small class="text-muted"><?php echo format_bitrate($videoBitrate); ?></small>
                        </td>
                        <td>
                            <span class="badge bg-secondary codec-badge"><?php echo getCodecDisplay($audioCodec); ?></span>
                            <br>
                            <small class="text-muted"><?php echo format_bitrate($audioBitrate); ?></small>
                        </td>
                        <td>
                            <?php if ($inputAddr && $inputAddr !== ':'): ?>
                            <small class="text-muted font-monospace"><?php echo htmlspecialchars($inputAddr); ?></small>
                            <?php else: ?>
                            <small class="text-muted">-</small>
                            <?php endif; ?>
                        </td>
                        <td>
                            <?php if ($outputAddr && $outputAddr !== ':'): ?>
                            <small class="text-muted font-monospace"><?php echo htmlspecialchars($outputAddr); ?></small>
                            <?php else: ?>
                            <small class="text-muted">-</small>
                            <?php endif; ?>
                        </td>
                        <td>
                            <div class="bitrate-cell" id="bitrate-<?php echo $transcoder['id']; ?>">
                                <span class="bitrate-total text-success fw-bold">-</span>
                            </div>
                        </td>
                        <td>
                            <div class="btn-group btn-group-sm">
                                <?php if ($transcoder['status'] === 'running'): ?>
                                <button class="btn btn-outline-warning" onclick="stopTranscoder('<?php echo $transcoder['id']; ?>')" title="Stop">
                                    <i class="bi bi-stop-fill"></i>
                                </button>
                                <button class="btn btn-outline-info" onclick="showPreview('<?php echo $transcoder['id']; ?>', '<?php echo htmlspecialchars($transcoder['name']); ?>')" title="Monitor">
                                    <i class="bi bi-graph-up"></i>
                                </button>
                                <?php else: ?>
                                <button class="btn btn-outline-success" onclick="startTranscoder('<?php echo $transcoder['id']; ?>')" title="Start">
                                    <i class="bi bi-play-fill"></i>
                                </button>
                                <?php endif; ?>
                                <button class="btn btn-outline-secondary" onclick="editTranscoder('<?php echo $transcoder['id']; ?>')" title="Edit">
                                    <i class="bi bi-gear"></i>
                                </button>
                                <button class="btn btn-outline-danger" onclick="deleteTranscoder('<?php echo $transcoder['id']; ?>')" title="Delete">
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

<!-- Preview Modal -->
<div class="modal fade" id="previewModal" tabindex="-1">
    <div class="modal-dialog modal-xl">
        <div class="modal-content">
            <div class="modal-header">
                <h5 class="modal-title"><i class="bi bi-graph-up me-2"></i>Transcoder Monitor - <span id="previewName"></span></h5>
                <button type="button" class="btn-close" data-bs-dismiss="modal"></button>
            </div>
            <div class="modal-body">
                <input type="hidden" id="previewId">
                <input type="hidden" id="previewApiPort" value="">

                <!-- Video Player Section -->
                <div class="card mb-3">
                    <div class="card-header py-2 d-flex justify-content-between align-items-center">
                        <strong><i class="bi bi-play-circle me-1"></i>Output Preview</strong>
                        <div>
                            <span id="playerStatus" class="badge bg-secondary me-2">Stopped</span>
                            <button class="btn btn-sm btn-success" id="startPlayerBtn" onclick="startOutputPlayer()">
                                <i class="bi bi-play-fill me-1"></i>Start
                            </button>
                            <button class="btn btn-sm btn-danger d-none" id="stopPlayerBtn" onclick="stopOutputPlayer()">
                                <i class="bi bi-stop-fill me-1"></i>Stop
                            </button>
                        </div>
                    </div>
                    <div class="card-body p-0">
                        <div class="ratio ratio-16x9 bg-dark position-relative" style="max-height: 300px;">
                            <div id="videoLoadingOverlay" class="position-absolute top-0 start-0 w-100 h-100 d-flex justify-content-center align-items-center">
                                <div class="text-center text-white">
                                    <i class="bi bi-tv fs-1 text-muted"></i>
                                    <div id="videoStatusText" class="mt-2 text-muted">Click Start to preview output</div>
                                </div>
                            </div>
                            <video id="outputVideo" class="w-100 h-100 d-none" controls autoplay muted playsinline></video>
                        </div>
                    </div>
                </div>

                <!-- Status Bar -->
                <div class="alert alert-info mb-3 py-2" id="monitorStatus">
                    <i class="bi bi-activity me-1"></i>
                    <span id="monitorStatusText">Connecting...</span>
                </div>

                <!-- Format Information Row -->
                <div class="row mb-3">
                    <!-- Input Format -->
                    <div class="col-md-6">
                        <div class="card h-100">
                            <div class="card-header py-2 bg-info bg-opacity-10">
                                <strong><i class="bi bi-box-arrow-in-right me-1"></i>Input Format</strong>
                                <small class="text-muted ms-2" id="inputSourceName"></small>
                            </div>
                            <div class="card-body py-2">
                                <div class="row small">
                                    <div class="col-6">
                                        <div class="mb-1"><span class="text-muted">Video:</span> <span id="inputVideoCodec">-</span></div>
                                        <div class="mb-1"><span class="text-muted">Resolution:</span> <span id="inputResolution">-</span></div>
                                    </div>
                                    <div class="col-6">
                                        <div class="mb-1"><span class="text-muted">Audio:</span> <span id="inputAudioCodec">-</span></div>
                                        <div><span class="text-muted">Channels:</span> <span id="inputAudioChannels">-</span></div>
                                    </div>
                                </div>
                            </div>
                        </div>
                    </div>
                    <!-- Output Format -->
                    <div class="col-md-6">
                        <div class="card h-100">
                            <div class="card-header py-2 bg-success bg-opacity-10">
                                <strong><i class="bi bi-box-arrow-right me-1"></i>Output Format</strong>
                                <small class="text-muted ms-2" id="outputAddressName"></small>
                            </div>
                            <div class="card-body py-2">
                                <div class="row small">
                                    <div class="col-6">
                                        <div class="mb-1"><span class="text-muted">Video:</span> <span id="outputVideoCodec">-</span></div>
                                        <div class="mb-1"><span class="text-muted">Resolution:</span> <span id="outputResolution">-</span></div>
                                    </div>
                                    <div class="col-6">
                                        <div class="mb-1"><span class="text-muted">Audio:</span> <span id="outputAudioCodec">-</span></div>
                                        <div><span class="text-muted">Bitrate:</span> <span id="outputAudioBitrateConfig">-</span></div>
                                    </div>
                                </div>
                            </div>
                        </div>
                    </div>
                </div>

                <!-- Current Bitrate Stats -->
                <div class="row mb-3">
                    <div class="col-md-6">
                        <div class="card">
                            <div class="card-header py-2">
                                <strong><i class="bi bi-box-arrow-in-right me-1 text-info"></i>Input Bitrate</strong>
                            </div>
                            <div class="card-body py-2">
                                <div class="d-flex justify-content-between align-items-center mb-1">
                                    <span class="text-muted small">Video</span>
                                    <span class="fw-bold text-info" id="monitorInputVideoBitrate">-</span>
                                </div>
                                <div class="d-flex justify-content-between align-items-center">
                                    <span class="text-muted small">Audio</span>
                                    <span class="fw-bold text-info" id="monitorInputAudioBitrate">-</span>
                                </div>
                            </div>
                        </div>
                    </div>
                    <div class="col-md-6">
                        <div class="card">
                            <div class="card-header py-2">
                                <strong><i class="bi bi-box-arrow-right me-1 text-success"></i>Output Bitrate</strong>
                            </div>
                            <div class="card-body py-2">
                                <div class="d-flex justify-content-between align-items-center mb-1">
                                    <span class="text-muted small">Video</span>
                                    <span class="fw-bold text-success" id="monitorOutputVideoBitrate">-</span>
                                </div>
                                <div class="d-flex justify-content-between align-items-center">
                                    <span class="text-muted small">Audio</span>
                                    <span class="fw-bold text-success" id="monitorOutputAudioBitrate">-</span>
                                </div>
                            </div>
                        </div>
                    </div>
                </div>

                <!-- Bitrate Graph -->
                <div class="card mb-3">
                    <div class="card-header py-2 d-flex justify-content-between align-items-center">
                        <strong><i class="bi bi-graph-up me-1"></i>Bitrate History</strong>
                        <small class="text-muted">
                            <span id="graphStatus" class="badge bg-success">Live</span>
                            Last update: <span id="graphLastUpdate">-</span>
                        </small>
                    </div>
                    <div class="card-body">
                        <div class="row mb-2">
                            <div class="col-3 text-center">
                                <span style="color: #0dcaf0;" class="fw-bold small">● In Video</span>
                            </div>
                            <div class="col-3 text-center">
                                <span style="color: #6edff6;" class="fw-bold small">● In Audio</span>
                            </div>
                            <div class="col-3 text-center">
                                <span style="color: #198754;" class="fw-bold small">● Out Video</span>
                            </div>
                            <div class="col-3 text-center">
                                <span style="color: #75b798;" class="fw-bold small">● Out Audio</span>
                            </div>
                        </div>
                        <div class="position-relative" style="height: 250px;">
                            <canvas id="bitrateChart"></canvas>
                        </div>
                    </div>
                </div>

                <!-- A/V Sync Placeholder -->
                <div class="card">
                    <div class="card-header py-2">
                        <strong><i class="bi bi-soundwave me-1"></i>A/V Sync Monitor</strong>
                        <span class="badge bg-secondary ms-2">Pending</span>
                    </div>
                    <div class="card-body">
                        <div class="text-center text-muted py-3">
                            <i class="bi bi-clock-history me-2"></i>
                            A/V sync monitoring requires integration with output analyzer.
                            <br><small>Coming in future update.</small>
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

<script src="https://cdn.jsdelivr.net/npm/chart.js@4.4.1/dist/chart.umd.min.js"></script>
<script>
let metricsInterval = null;
let previewModal = null;
let bitrateChart = null;
let previewInterval = null;
let inputVideoHistory = [];
let inputAudioHistory = [];
let outputVideoHistory = [];
let outputAudioHistory = [];
const MAX_HISTORY_POINTS = 60;

// Filter transcoders
function filterTranscoders() {
    const search = document.getElementById('searchInput').value.toLowerCase();
    const codec = document.getElementById('codecFilter').value.toLowerCase();
    const status = document.getElementById('statusFilter').value.toLowerCase();

    const rows = document.querySelectorAll('.transcoder-row');
    let visibleCount = 0;

    rows.forEach(row => {
        const name = row.dataset.name || '';
        const rowCodec = row.dataset.codec || '';
        const rowStatus = row.dataset.status || '';

        const matchSearch = !search || name.includes(search);
        const matchCodec = !codec || rowCodec === codec;
        const matchStatus = !status || rowStatus === status;

        if (matchSearch && matchCodec && matchStatus) {
            row.style.display = '';
            visibleCount++;
        } else {
            row.style.display = 'none';
        }
    });

    document.getElementById('transcoderCount').textContent = visibleCount + ' transcoders';
}

// Start transcoder
async function startTranscoder(id) {
    try {
        const response = await fetch(`api/transcoders.php?action=start&id=${id}`, { method: 'POST' });
        const data = await response.json();
        if (data.success) {
            location.reload();
        } else {
            alert('Failed to start: ' + (data.error || 'Unknown error'));
        }
    } catch (e) {
        alert('Error: ' + e.message);
    }
}

// Stop transcoder
async function stopTranscoder(id) {
    try {
        const response = await fetch(`api/transcoders.php?action=stop&id=${id}`, { method: 'POST' });
        const data = await response.json();
        if (data.success) {
            location.reload();
        } else {
            alert('Failed to stop: ' + (data.error || 'Unknown error'));
        }
    } catch (e) {
        alert('Error: ' + e.message);
    }
}

// Delete transcoder
async function deleteTranscoder(id) {
    if (!confirm('Are you sure you want to delete this transcoder?')) return;

    try {
        const response = await fetch(`api/transcoders.php?action=delete&id=${id}`, { method: 'POST' });
        const data = await response.json();
        if (data.success) {
            location.reload();
        } else {
            alert('Failed to delete: ' + (data.error || 'Unknown error'));
        }
    } catch (e) {
        alert('Error: ' + e.message);
    }
}

// Edit transcoder
function editTranscoder(id) {
    window.location.href = `transcoders-edit.php?id=${id}`;
}

// Format bitrate for display
function formatBitrate(bps) {
    if (!bps || bps === 0) return '-';
    if (bps >= 1000000) {
        return (bps / 1000000).toFixed(2) + ' Mbps';
    } else if (bps >= 1000) {
        return (bps / 1000).toFixed(0) + ' Kbps';
    }
    return bps + ' bps';
}

// Initialize bitrate chart
function initBitrateChart() {
    const ctx = document.getElementById('bitrateChart').getContext('2d');

    if (bitrateChart) {
        bitrateChart.destroy();
    }

    bitrateChart = new Chart(ctx, {
        type: 'line',
        data: {
            labels: [],
            datasets: [
                {
                    label: 'Input Video',
                    data: [],
                    borderColor: '#0dcaf0',
                    backgroundColor: 'rgba(13, 202, 240, 0.1)',
                    fill: false,
                    tension: 0.3,
                    pointRadius: 0,
                    borderWidth: 2
                },
                {
                    label: 'Input Audio',
                    data: [],
                    borderColor: '#6edff6',
                    backgroundColor: 'rgba(110, 223, 246, 0.1)',
                    fill: false,
                    tension: 0.3,
                    pointRadius: 0,
                    borderWidth: 1,
                    borderDash: [5, 5]
                },
                {
                    label: 'Output Video',
                    data: [],
                    borderColor: '#198754',
                    backgroundColor: 'rgba(25, 135, 84, 0.1)',
                    fill: false,
                    tension: 0.3,
                    pointRadius: 0,
                    borderWidth: 2
                },
                {
                    label: 'Output Audio',
                    data: [],
                    borderColor: '#75b798',
                    backgroundColor: 'rgba(117, 183, 152, 0.1)',
                    fill: false,
                    tension: 0.3,
                    pointRadius: 0,
                    borderWidth: 1,
                    borderDash: [5, 5]
                }
            ]
        },
        options: {
            responsive: true,
            maintainAspectRatio: false,
            interaction: {
                intersect: false,
                mode: 'index'
            },
            plugins: {
                legend: { display: false },
                tooltip: {
                    callbacks: {
                        label: function(context) {
                            return context.dataset.label + ': ' + formatBitrate(context.parsed.y);
                        }
                    }
                }
            },
            scales: {
                x: {
                    display: true,
                    ticks: { maxTicksLimit: 10 }
                },
                y: {
                    display: true,
                    beginAtZero: true,
                    ticks: {
                        callback: function(value) {
                            return formatBitrate(value);
                        }
                    }
                }
            }
        }
    });
}

// Fetch metrics for preview modal
async function loadPreviewMetrics() {
    const id = document.getElementById('previewId').value;
    if (!id) return;

    try {
        const response = await fetch(`api/transcoders.php?action=all_metrics`);
        const data = await response.json();

        if (data.success && data.transcoders && data.transcoders[id]) {
            const metrics = data.transcoders[id];

            // Update status
            const statusEl = document.getElementById('monitorStatus');
            const statusText = document.getElementById('monitorStatusText');

            if (metrics.status === 'running') {
                statusEl.className = 'alert alert-success mb-3 py-2';
                statusText.textContent = 'Transcoder running - receiving data';
            } else if (metrics.status === 'stopped') {
                statusEl.className = 'alert alert-secondary mb-3 py-2';
                statusText.textContent = 'Transcoder stopped';
            } else {
                statusEl.className = 'alert alert-warning mb-3 py-2';
                statusText.textContent = 'Transcoder offline or not responding';
            }

            // Update input format display - fetch from input's preview_media_info API
            if (metrics.source_service) {
                document.getElementById('inputSourceName').textContent = '(' + metrics.source_service + ')';
                // Load detailed input format info (only once per modal open)
                if (!window.inputFormatLoaded) {
                    window.inputFormatLoaded = true;
                    loadInputMediaInfo(metrics.source_service);
                }
            }

            // Update output format display - probe actual stream
            if (metrics.output_address) {
                document.getElementById('outputAddressName').textContent = '(' + metrics.output_address + ')';
                // Store for player
                window.currentOutputAddress = metrics.output_address;
                if (!window.outputFormatLoaded) {
                    window.outputFormatLoaded = true;
                    loadOutputMediaInfo(metrics.output_address);
                }
            }

            // Store api_port for player
            if (metrics.api_port) {
                document.getElementById('previewApiPort').value = metrics.api_port;
            }

            // Update bitrate displays
            document.getElementById('monitorInputVideoBitrate').textContent = formatBitrate(metrics.input_video_bitrate || 0);
            document.getElementById('monitorInputAudioBitrate').textContent = formatBitrate(metrics.input_audio_bitrate || 0);
            document.getElementById('monitorOutputVideoBitrate').textContent = formatBitrate(metrics.output_video_bitrate || 0);
            document.getElementById('monitorOutputAudioBitrate').textContent = formatBitrate(metrics.output_audio_bitrate || 0);

            // Add to history (4 series: input video, input audio, output video, output audio)
            inputVideoHistory.push(metrics.input_video_bitrate || 0);
            inputAudioHistory.push(metrics.input_audio_bitrate || 0);
            outputVideoHistory.push(metrics.output_video_bitrate || 0);
            outputAudioHistory.push(metrics.output_audio_bitrate || 0);

            if (inputVideoHistory.length > MAX_HISTORY_POINTS) {
                inputVideoHistory.shift();
                inputAudioHistory.shift();
                outputVideoHistory.shift();
                outputAudioHistory.shift();
            }

            // Update chart
            if (bitrateChart) {
                const labels = Array(inputVideoHistory.length).fill('').map((_, i) => {
                    const idx = inputVideoHistory.length - 1 - i;
                    return idx % 12 === 0 ? `-${Math.floor(idx * 5 / 60)}m` : '';
                }).reverse();

                bitrateChart.data.labels = labels;
                bitrateChart.data.datasets[0].data = [...inputVideoHistory];
                bitrateChart.data.datasets[1].data = [...inputAudioHistory];
                bitrateChart.data.datasets[2].data = [...outputVideoHistory];
                bitrateChart.data.datasets[3].data = [...outputAudioHistory];
                bitrateChart.update('none');
            }

            // Update timestamp
            document.getElementById('graphLastUpdate').textContent = new Date().toLocaleTimeString();
            document.getElementById('graphStatus').className = 'badge bg-success';
            document.getElementById('graphStatus').textContent = 'Live';
        } else {
            document.getElementById('graphStatus').className = 'badge bg-danger';
            document.getElementById('graphStatus').textContent = 'Offline';
        }
    } catch (e) {
        console.error('Failed to fetch preview metrics:', e);
        document.getElementById('graphStatus').className = 'badge bg-warning';
        document.getElementById('graphStatus').textContent = 'Error';
    }
}

// Load input media info via the same API used by inputs page
async function loadInputMediaInfo(inputId) {
    try {
        const response = await fetch(`api/inputs.php?action=preview_media_info&id=${inputId}`);
        const data = await response.json();

        if (!data.success) {
            console.log('Failed to load input media info:', data.error);
            return;
        }

        // Update video info
        if (data.video) {
            document.getElementById('inputVideoCodec').textContent =
                (data.video.codec || '-').toUpperCase() + (data.video.profile ? ` (${data.video.profile})` : '');
            document.getElementById('inputResolution').textContent =
                data.video.width && data.video.height ? `${data.video.width}x${data.video.height}` : '-';
        }

        // Update audio info (use first track)
        if (data.audio && data.audio.length > 0) {
            const track = data.audio[0];
            document.getElementById('inputAudioCodec').textContent =
                (track.codec || '-').toUpperCase() + (track.profile ? ` (${track.profile})` : '');
            document.getElementById('inputAudioChannels').textContent =
                track.channels ? `${track.channels}ch` : '-';
        }
    } catch (e) {
        console.error('Failed to load input media info:', e);
    }
}

// Load output media info via ffprobe API
async function loadOutputMediaInfo(outputAddress) {
    try {
        const response = await fetch(`api/transcoders.php?action=probe_stream&address=${encodeURIComponent(outputAddress)}`);
        const data = await response.json();

        if (!data.success) {
            console.log('Failed to load output media info:', data.error);
            document.getElementById('outputVideoCodec').textContent = 'N/A';
            document.getElementById('outputResolution').textContent = 'N/A';
            document.getElementById('outputAudioCodec').textContent = 'N/A';
            document.getElementById('outputAudioBitrateConfig').textContent = 'N/A';
            return;
        }

        // Update video info
        if (data.video) {
            document.getElementById('outputVideoCodec').textContent =
                (data.video.codec || '-').toUpperCase() + (data.video.profile ? ` (${data.video.profile})` : '');
            document.getElementById('outputResolution').textContent =
                data.video.width && data.video.height ? `${data.video.width}x${data.video.height}` : '-';
        }

        // Update audio info (use first track)
        if (data.audio && data.audio.length > 0) {
            const track = data.audio[0];
            document.getElementById('outputAudioCodec').textContent =
                (track.codec || '-').toUpperCase() + (track.profile ? ` (${track.profile})` : '');
            document.getElementById('outputAudioBitrateConfig').textContent =
                track.bit_rate ? formatBitrate(parseInt(track.bit_rate)) : '-';
        }
    } catch (e) {
        console.error('Failed to load output media info:', e);
        document.getElementById('outputVideoCodec').textContent = 'Error';
    }
}

// Show preview modal
function showPreview(id, name) {
    document.getElementById('previewId').value = id;
    document.getElementById('previewName').textContent = name;
    document.getElementById('previewApiPort').value = '';

    // Reset format loaded flags
    window.inputFormatLoaded = false;
    window.outputFormatLoaded = false;
    window.currentOutputAddress = null;

    // Reset player state
    if (outputHlsPlayer) {
        outputHlsPlayer.destroy();
        outputHlsPlayer = null;
    }
    outputPlayerRunning = false;
    document.getElementById('outputVideo').classList.add('d-none');
    document.getElementById('videoLoadingOverlay').classList.remove('d-none');
    document.getElementById('startPlayerBtn').classList.remove('d-none');
    document.getElementById('stopPlayerBtn').classList.add('d-none');
    document.getElementById('playerStatus').className = 'badge bg-secondary me-2';
    document.getElementById('playerStatus').textContent = 'Stopped';
    document.getElementById('videoStatusText').textContent = 'Click Start to preview output';

    // Reset history (4 series)
    inputVideoHistory = [];
    inputAudioHistory = [];
    outputVideoHistory = [];
    outputAudioHistory = [];

    // Reset format displays
    document.getElementById('inputSourceName').textContent = '';
    document.getElementById('inputVideoCodec').textContent = 'Loading...';
    document.getElementById('inputResolution').textContent = '-';
    document.getElementById('inputAudioCodec').textContent = 'Loading...';
    document.getElementById('inputAudioChannels').textContent = '-';
    document.getElementById('outputAddressName').textContent = '';
    document.getElementById('outputVideoCodec').textContent = 'Loading...';
    document.getElementById('outputResolution').textContent = '-';
    document.getElementById('outputAudioCodec').textContent = 'Loading...';
    document.getElementById('outputAudioBitrateConfig').textContent = '-';

    // Reset bitrate displays
    document.getElementById('monitorInputVideoBitrate').textContent = '-';
    document.getElementById('monitorInputAudioBitrate').textContent = '-';
    document.getElementById('monitorOutputVideoBitrate').textContent = '-';
    document.getElementById('monitorOutputAudioBitrate').textContent = '-';

    // Initialize chart
    initBitrateChart();

    // Load initial metrics
    loadPreviewMetrics();

    // Start polling
    if (previewInterval) clearInterval(previewInterval);
    previewInterval = setInterval(loadPreviewMetrics, 5000);

    // Show modal
    if (!previewModal) {
        previewModal = new bootstrap.Modal(document.getElementById('previewModal'));
    }
    previewModal.show();
}

// Cleanup on modal close
document.getElementById('previewModal').addEventListener('hidden.bs.modal', function() {
    if (previewInterval) {
        clearInterval(previewInterval);
        previewInterval = null;
    }

    // Stop player if running
    if (outputPlayerRunning) {
        stopOutputPlayer();
    }

    inputVideoHistory = [];
    inputAudioHistory = [];
    outputVideoHistory = [];
    outputAudioHistory = [];
});

// Fetch all transcoder metrics
async function fetchAllMetrics() {
    try {
        const response = await fetch('api/transcoders.php?action=all_metrics');
        const data = await response.json();

        if (data.success && data.transcoders) {
            for (const [transcoderId, metrics] of Object.entries(data.transcoders)) {
                updateTranscoderMetrics(transcoderId, metrics);
            }
        }
    } catch (e) {
        console.error('Failed to fetch metrics:', e);
    }
}

// Update metrics display for a single transcoder
function updateTranscoderMetrics(transcoderId, metrics) {
    const bitrateCell = document.getElementById(`bitrate-${transcoderId}`);
    const statusDot = document.getElementById(`status-${transcoderId}`);

    if (!bitrateCell) return;

    const totalSpan = bitrateCell.querySelector('.bitrate-total');

    if (!metrics || metrics.status === 'offline') {
        bitrateCell.classList.add('offline');
        if (totalSpan) totalSpan.textContent = '-';
        return;
    }

    bitrateCell.classList.remove('offline');

    // Use output_total_bitrate or video_bitrate (which holds total when using TS bitrate mode)
    const totalBitrate = metrics.output_total_bitrate || metrics.video_bitrate || 0;
    if (totalSpan) totalSpan.textContent = formatBitrate(totalBitrate);

    // Update status dot if needed
    if (statusDot && metrics.status) {
        statusDot.className = 'status-dot status-' + metrics.status;
        statusDot.title = metrics.status.charAt(0).toUpperCase() + metrics.status.slice(1);
    }
}

// Start metrics polling on page load
document.addEventListener('DOMContentLoaded', function() {
    // Fetch metrics immediately and then every 5 seconds
    fetchAllMetrics();
    metricsInterval = setInterval(fetchAllMetrics, 5000);
});

// Clean up on page unload
window.addEventListener('beforeunload', function() {
    if (metricsInterval) clearInterval(metricsInterval);
});
// Output player state
let outputHlsPlayer = null;
let outputPlayerRunning = false;

// Start output player preview
async function startOutputPlayer() {
    const id = document.getElementById('previewId').value;
    const apiPort = document.getElementById('previewApiPort').value;
    const outputAddress = window.currentOutputAddress;

    if (!outputAddress) {
        document.getElementById('videoStatusText').textContent = 'No output address available';
        return;
    }

    document.getElementById('startPlayerBtn').classList.add('d-none');
    document.getElementById('stopPlayerBtn').classList.remove('d-none');
    document.getElementById('playerStatus').className = 'badge bg-warning me-2';
    document.getElementById('playerStatus').textContent = 'Starting...';
    document.getElementById('videoStatusText').textContent = 'Starting preview...';

    try {
        // Start player_preview for the output stream
        const outputDir = `/var/www/caritrans/public/hls/transcoder-${id}`;
        const previewPort = parseInt(apiPort) + 100; // Use api_port + 100 for preview

        const response = await fetch('api/transcoders.php?action=start_preview', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({
                id: id,
                input_address: outputAddress,
                output_dir: outputDir,
                api_port: previewPort
            })
        });

        const data = await response.json();

        if (data.success) {
            outputPlayerRunning = true;
            document.getElementById('playerStatus').className = 'badge bg-info me-2';
            document.getElementById('playerStatus').textContent = 'Loading...';
            document.getElementById('videoStatusText').textContent = 'Loading player...';

            // Wait a moment for HLS segments to be generated
            setTimeout(() => {
                const playlistUrl = `/hls/transcoder-${id}/playlist.m3u8`;
                initOutputHlsPlayer(playlistUrl);
            }, 2000);
        } else {
            throw new Error(data.error || 'Failed to start preview');
        }
    } catch (e) {
        console.error('Failed to start output player:', e);
        document.getElementById('playerStatus').className = 'badge bg-danger me-2';
        document.getElementById('playerStatus').textContent = 'Error';
        document.getElementById('videoStatusText').textContent = 'Failed: ' + e.message;
        document.getElementById('startPlayerBtn').classList.remove('d-none');
        document.getElementById('stopPlayerBtn').classList.add('d-none');
    }
}

// Stop output player preview
async function stopOutputPlayer() {
    const id = document.getElementById('previewId').value;
    const apiPort = document.getElementById('previewApiPort').value;
    const previewPort = parseInt(apiPort) + 100;

    // Destroy HLS player
    if (outputHlsPlayer) {
        outputHlsPlayer.destroy();
        outputHlsPlayer = null;
    }

    const video = document.getElementById('outputVideo');
    video.classList.add('d-none');
    document.getElementById('videoLoadingOverlay').classList.remove('d-none');

    try {
        await fetch(`api/transcoders.php?action=stop_preview&api_port=${previewPort}`, {
            method: 'POST'
        });
    } catch (e) {
        console.error('Failed to stop preview:', e);
    }

    outputPlayerRunning = false;
    document.getElementById('startPlayerBtn').classList.remove('d-none');
    document.getElementById('stopPlayerBtn').classList.add('d-none');
    document.getElementById('playerStatus').className = 'badge bg-secondary me-2';
    document.getElementById('playerStatus').textContent = 'Stopped';
    document.getElementById('videoStatusText').textContent = 'Click Start to preview output';
}

// Initialize HLS player for output
function initOutputHlsPlayer(playlistUrl) {
    const video = document.getElementById('outputVideo');

    if (outputHlsPlayer) {
        outputHlsPlayer.destroy();
        outputHlsPlayer = null;
    }

    if (Hls.isSupported()) {
        outputHlsPlayer = new Hls({
            liveSyncDurationCount: 3,
            liveMaxLatencyDurationCount: 6
        });

        outputHlsPlayer.loadSource(playlistUrl);
        outputHlsPlayer.attachMedia(video);

        outputHlsPlayer.on(Hls.Events.MANIFEST_PARSED, function() {
            document.getElementById('videoLoadingOverlay').classList.add('d-none');
            video.classList.remove('d-none');
            document.getElementById('playerStatus').className = 'badge bg-success me-2';
            document.getElementById('playerStatus').textContent = 'Playing';
            video.play();
        });

        outputHlsPlayer.on(Hls.Events.ERROR, function(event, data) {
            console.error('HLS error:', data);
            if (data.fatal) {
                document.getElementById('playerStatus').className = 'badge bg-danger me-2';
                document.getElementById('playerStatus').textContent = 'Error';
                document.getElementById('videoStatusText').textContent = 'Playback error';
            }
        });
    } else if (video.canPlayType('application/vnd.apple.mpegurl')) {
        // Safari native HLS
        video.src = playlistUrl;
        video.addEventListener('loadedmetadata', function() {
            document.getElementById('videoLoadingOverlay').classList.add('d-none');
            video.classList.remove('d-none');
            document.getElementById('playerStatus').className = 'badge bg-success me-2';
            document.getElementById('playerStatus').textContent = 'Playing';
            video.play();
        });
    } else {
        document.getElementById('videoStatusText').textContent = 'HLS not supported in this browser';
        document.getElementById('playerStatus').className = 'badge bg-danger me-2';
        document.getElementById('playerStatus').textContent = 'Unsupported';
    }
}
</script>

<!-- HLS.js Library -->
<script src="https://cdnjs.cloudflare.com/ajax/libs/hls.js/1.6.0-beta.1.0.canary.10759/hls.min.js"></script>

<?php include __DIR__ . '/../templates/footer.php'; ?>
