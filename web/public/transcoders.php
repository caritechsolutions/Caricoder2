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
                        <th style="width: 180px;">Bitrate (V/A)</th>
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
                                <span class="bitrate-video">-</span> / <span class="bitrate-audio">-</span>
                            </div>
                        </td>
                        <td>
                            <div class="btn-group btn-group-sm">
                                <?php if ($transcoder['status'] === 'running'): ?>
                                <button class="btn btn-outline-warning" onclick="stopTranscoder('<?php echo $transcoder['id']; ?>')" title="Stop">
                                    <i class="bi bi-stop-fill"></i>
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

<script>
let metricsInterval = null;

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

    const videoSpan = bitrateCell.querySelector('.bitrate-video');
    const audioSpan = bitrateCell.querySelector('.bitrate-audio');

    if (!metrics || metrics.status === 'offline') {
        bitrateCell.classList.add('offline');
        if (videoSpan) videoSpan.textContent = '-';
        if (audioSpan) audioSpan.textContent = '-';
        return;
    }

    bitrateCell.classList.remove('offline');

    if (videoSpan) videoSpan.textContent = formatBitrate(metrics.video_bitrate || 0);
    if (audioSpan) audioSpan.textContent = formatBitrate(metrics.audio_bitrate || 0);

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
</script>

<?php include __DIR__ . '/../templates/footer.php'; ?>
