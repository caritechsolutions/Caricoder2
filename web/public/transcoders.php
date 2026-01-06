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
    // When scaling is disabled, resolution is preserved from input
    return '';
}

// Helper function to check if transcoder is ABR mode
function isAbrMode($config) {
    return !empty($config['abr']['enabled']) && config_bool($config['abr']['enabled']);
}

// Helper function to get ABR variants info
function getAbrVariants($config) {
    $variants = [];
    if (!isAbrMode($config)) {
        return $variants;
    }
    $variant_count = intval($config['abr']['variant_count'] ?? 0);
    for ($i = 0; $i < $variant_count; $i++) {
        $variants[] = [
            'width' => $config['abr']["variant_{$i}_width"] ?? 1920,
            'height' => $config['abr']["variant_{$i}_height"] ?? 1080,
            'bitrate' => $config['abr']["variant_{$i}_bitrate"] ?? 5000000,
            'video_pid' => $config['abr']["variant_{$i}_video_pid"] ?? (100 + $i * 100)
        ];
    }
    return $variants;
}

// Helper function to get total configured bitrate for ABR
function getAbrTotalBitrate($config) {
    $total = 0;
    $variants = getAbrVariants($config);
    foreach ($variants as $variant) {
        $total += $variant['bitrate'];
    }
    // Add audio bitrate
    $total += intval($config['audio']['bitrate'] ?? 128000);
    return $total;
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

/* A/V Sync Styles */
.avsync-stat-card {
    background: #f8f9fa;
    border-radius: 8px;
    padding: 10px;
    text-align: center;
}
.avsync-stat-card .stat-label {
    font-size: 0.75rem;
    color: #6c757d;
    text-transform: uppercase;
    letter-spacing: 0.5px;
}
.avsync-stat-card .stat-value {
    font-size: 1.25rem;
    font-weight: 600;
    color: #212529;
}
.avsync-stat-card .stat-sublabel {
    font-size: 0.7rem;
    color: #adb5bd;
}
.avsync-stat-card .stat-value.status-ok { color: #16a34a; }
.avsync-stat-card .stat-value.status-warning { color: #d97706; }
.avsync-stat-card .stat-value.status-error { color: #dc2626; }
.badge.avsync-ok { background-color: #16a34a !important; }
.badge.avsync-warning { background-color: #d97706 !important; }
.badge.avsync-error { background-color: #dc2626 !important; }

/* HLS Player Stats Panel */
.player-stats-panel {
    background: linear-gradient(135deg, #1a1d24 0%, #2d3748 100%);
    padding: 16px;
    border-top: 1px solid rgba(255,255,255,0.1);
}

.stats-grid {
    display: grid;
    grid-template-columns: repeat(4, 1fr);
    gap: 12px;
}

.stats-grid-2col {
    grid-template-columns: repeat(2, 1fr);
}

.stat-card {
    background: rgba(255,255,255,0.05);
    border-radius: 12px;
    padding: 14px;
    border: 1px solid rgba(255,255,255,0.08);
    backdrop-filter: blur(10px);
    transition: all 0.2s ease;
}

.stat-card:hover {
    background: rgba(255,255,255,0.08);
    border-color: rgba(255,255,255,0.15);
}

.stat-header {
    display: flex;
    align-items: center;
    gap: 6px;
    color: #9ca3af;
    font-size: 0.75rem;
    text-transform: uppercase;
    letter-spacing: 0.5px;
    margin-bottom: 10px;
}

.stat-header i {
    font-size: 0.85rem;
    opacity: 0.7;
}

.stat-value-large {
    font-size: 1.75rem;
    font-weight: 700;
    color: #fff;
    line-height: 1.1;
}

.stat-unit {
    font-size: 0.9rem;
    font-weight: 400;
    color: #9ca3af;
    margin-left: 2px;
}

.stat-label {
    font-size: 0.7rem;
    color: #6b7280;
    margin-top: 4px;
}

/* Buffer Gauge */
.stat-gauge {
    display: flex;
    align-items: center;
    gap: 10px;
}

.gauge-bar {
    flex: 1;
    height: 8px;
    background: rgba(255,255,255,0.1);
    border-radius: 4px;
    overflow: hidden;
}

.gauge-fill {
    height: 100%;
    background: linear-gradient(90deg, #10b981 0%, #34d399 100%);
    border-radius: 4px;
    transition: width 0.3s ease, background 0.3s ease;
}

.gauge-fill.warning {
    background: linear-gradient(90deg, #f59e0b 0%, #fbbf24 100%);
}

.gauge-fill.critical {
    background: linear-gradient(90deg, #ef4444 0%, #f87171 100%);
}

.gauge-value {
    font-size: 1.1rem;
    font-weight: 600;
    color: #fff;
    min-width: 50px;
    text-align: right;
}

/* Sparkline */
.stat-sparkline {
    margin-top: 8px;
    height: 24px;
}

.stat-sparkline canvas {
    width: 100%;
}

/* Row Lists */
.stat-row-list {
    display: flex;
    flex-direction: column;
    gap: 8px;
}

.stat-row {
    display: flex;
    justify-content: space-between;
    align-items: center;
}

.stat-row-label {
    color: #9ca3af;
    font-size: 0.8rem;
}

.stat-row-value {
    color: #fff;
    font-weight: 600;
    font-size: 0.9rem;
    display: flex;
    align-items: center;
    gap: 6px;
}

/* Status Dots */
.status-dot {
    width: 8px;
    height: 8px;
    border-radius: 50%;
    display: inline-block;
}

.status-dot-ok {
    background: #10b981;
    box-shadow: 0 0 6px rgba(16, 185, 129, 0.5);
}

.status-dot-warning {
    background: #f59e0b;
    box-shadow: 0 0 6px rgba(245, 158, 11, 0.5);
}

.status-dot-error {
    background: #ef4444;
    box-shadow: 0 0 6px rgba(239, 68, 68, 0.5);
}

/* Quality Levels List */
.quality-levels-list {
    display: flex;
    flex-direction: column;
    gap: 8px;
    margin-top: 10px;
}

.quality-level-item {
    display: flex;
    align-items: center;
    gap: 10px;
    padding: 8px 10px;
    background: rgba(255,255,255,0.03);
    border-radius: 8px;
    transition: all 0.2s ease;
}

.quality-level-item.active {
    background: rgba(16, 185, 129, 0.15);
    border: 1px solid rgba(16, 185, 129, 0.3);
}

.quality-level-indicator {
    width: 10px;
    height: 10px;
    border-radius: 50%;
    border: 2px solid #4b5563;
    flex-shrink: 0;
}

.quality-level-item.active .quality-level-indicator {
    background: #10b981;
    border-color: #10b981;
    box-shadow: 0 0 8px rgba(16, 185, 129, 0.5);
}

.quality-level-info {
    flex: 1;
    display: flex;
    justify-content: space-between;
    align-items: center;
}

.quality-level-resolution {
    color: #fff;
    font-weight: 600;
    font-size: 0.85rem;
}

.quality-level-bitrate {
    color: #9ca3af;
    font-size: 0.8rem;
}

.quality-level-bar-container {
    flex: 1;
    max-width: 120px;
    height: 4px;
    background: rgba(255,255,255,0.1);
    border-radius: 2px;
    overflow: hidden;
}

.quality-level-bar {
    height: 100%;
    background: linear-gradient(90deg, #3b82f6 0%, #60a5fa 100%);
    border-radius: 2px;
}

.quality-level-item.active .quality-level-bar {
    background: linear-gradient(90deg, #10b981 0%, #34d399 100%);
}

/* Stats Toggle Button Active State */
#statsToggleBtn.active {
    background-color: #0dcaf0;
    border-color: #0dcaf0;
    color: #000;
}

/* Responsive adjustments */
@media (max-width: 768px) {
    .stats-grid {
        grid-template-columns: repeat(2, 1fr);
    }

    .stats-grid-2col {
        grid-template-columns: 1fr;
    }

    .stat-value-large {
        font-size: 1.4rem;
    }

    .quality-level-bar-container {
        display: none;
    }
}

@media (max-width: 480px) {
    .stats-grid {
        grid-template-columns: 1fr;
    }

    .player-stats-panel {
        padding: 12px;
    }

    .stat-card {
        padding: 12px;
    }
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
                        $isAbr = isAbrMode($config);
                        $abrVariants = $isAbr ? getAbrVariants($config) : [];
                        $videoBitrate = $isAbr ? getAbrTotalBitrate($config) - ($config['audio']['bitrate'] ?? 128000) : ($config['video']['bitrate'] ?? 0);
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
                            <?php if ($isAbr): ?>
                            <span class="badge bg-info codec-badge"><?php echo getCodecDisplay($videoCodec); ?></span>
                            <span class="badge bg-warning text-dark codec-badge">ABR</span>
                            <br>
                            <?php foreach ($abrVariants as $idx => $variant): ?>
                            <small class="text-muted"><?php echo $variant['width']; ?>x<?php echo $variant['height']; ?> @ <?php echo format_bitrate($variant['bitrate']); ?></small><?php if ($idx < count($abrVariants) - 1): ?><br><?php endif; ?>
                            <?php endforeach; ?>
                            <?php else: ?>
                            <span class="badge bg-info codec-badge"><?php echo getCodecDisplay($videoCodec); ?></span>
                            <?php if ($resolution): ?><small class="text-muted ms-1"><?php echo $resolution; ?></small><?php endif; ?>
                            <br>
                            <small class="text-muted"><?php echo format_bitrate($videoBitrate); ?></small>
                            <?php endif; ?>
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
                        <div class="d-flex align-items-center gap-2">
                            <span id="playerStatus" class="badge bg-secondary">Stopped</span>
                            <!-- Quality Selector (hidden until multiple levels available) -->
                            <select id="qualitySelector" class="form-select form-select-sm d-none" style="width: auto; min-width: 90px;" title="Video Quality">
                                <option value="-1">Auto</option>
                            </select>
                            <!-- CC Button (hidden until captions available) -->
                            <button id="ccBtn" class="btn btn-sm btn-outline-secondary d-none" title="Closed Captions" onclick="toggleClosedCaptions()">
                                <i class="bi bi-badge-cc"></i>
                            </button>
                            <!-- Stats Toggle Button -->
                            <button id="statsToggleBtn" class="btn btn-sm btn-outline-info d-none" title="Player Statistics" onclick="togglePlayerStats()">
                                <i class="bi bi-speedometer2"></i>
                            </button>
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

                        <!-- HLS Player Stats Panel (hidden by default) -->
                        <div id="playerStatsPanel" class="player-stats-panel d-none">
                            <div class="stats-grid">
                                <!-- Buffer Gauge -->
                                <div class="stat-card">
                                    <div class="stat-header">
                                        <i class="bi bi-collection"></i>
                                        <span>Buffer</span>
                                    </div>
                                    <div class="stat-gauge">
                                        <div class="gauge-bar">
                                            <div id="bufferGaugeFill" class="gauge-fill" style="width: 0%"></div>
                                        </div>
                                        <div class="gauge-value"><span id="bufferValue">0.0</span>s</div>
                                    </div>
                                    <div class="stat-label" id="bufferStatus">Waiting</div>
                                </div>

                                <!-- Latency -->
                                <div class="stat-card">
                                    <div class="stat-header">
                                        <i class="bi bi-clock-history"></i>
                                        <span>Latency</span>
                                    </div>
                                    <div class="stat-value-large">
                                        <span id="latencyValue">--</span><span class="stat-unit">s</span>
                                    </div>
                                    <div class="stat-label">Behind live</div>
                                </div>

                                <!-- Bandwidth -->
                                <div class="stat-card">
                                    <div class="stat-header">
                                        <i class="bi bi-speedometer"></i>
                                        <span>Bandwidth</span>
                                    </div>
                                    <div class="stat-value-large">
                                        <span id="bandwidthValue">--</span><span class="stat-unit">Mbps</span>
                                    </div>
                                    <div class="stat-sparkline">
                                        <canvas id="bandwidthSparkline" height="24"></canvas>
                                    </div>
                                </div>

                                <!-- Current Quality -->
                                <div class="stat-card">
                                    <div class="stat-header">
                                        <i class="bi bi-badge-hd"></i>
                                        <span>Quality</span>
                                    </div>
                                    <div class="stat-value-large">
                                        <span id="currentQualityValue">--</span>
                                    </div>
                                    <div class="stat-label" id="currentQualityBitrate">--</div>
                                </div>
                            </div>

                            <!-- Second Row: Frame Stats & Network -->
                            <div class="stats-grid stats-grid-2col mt-2">
                                <!-- Frame Stats -->
                                <div class="stat-card stat-card-wide">
                                    <div class="stat-header">
                                        <i class="bi bi-film"></i>
                                        <span>Frame Statistics</span>
                                    </div>
                                    <div class="stat-row-list">
                                        <div class="stat-row">
                                            <span class="stat-row-label">Decoded</span>
                                            <span class="stat-row-value" id="framesDecoded">0</span>
                                        </div>
                                        <div class="stat-row">
                                            <span class="stat-row-label">Dropped</span>
                                            <span class="stat-row-value">
                                                <span id="framesDropped">0</span>
                                                <span id="framesDroppedIndicator" class="status-dot status-dot-ok"></span>
                                            </span>
                                        </div>
                                        <div class="stat-row">
                                            <span class="stat-row-label">FPS</span>
                                            <span class="stat-row-value" id="currentFps">--</span>
                                        </div>
                                    </div>
                                </div>

                                <!-- Network Stats -->
                                <div class="stat-card stat-card-wide">
                                    <div class="stat-header">
                                        <i class="bi bi-wifi"></i>
                                        <span>Network</span>
                                    </div>
                                    <div class="stat-row-list">
                                        <div class="stat-row">
                                            <span class="stat-row-label">TTFB</span>
                                            <span class="stat-row-value"><span id="ttfbValue">--</span> ms</span>
                                        </div>
                                        <div class="stat-row">
                                            <span class="stat-row-label">Fragments</span>
                                            <span class="stat-row-value" id="fragmentsLoaded">0</span>
                                        </div>
                                        <div class="stat-row">
                                            <span class="stat-row-label">Stalls</span>
                                            <span class="stat-row-value">
                                                <span id="stallCount">0</span>
                                                <span id="stallIndicator" class="status-dot status-dot-ok"></span>
                                            </span>
                                        </div>
                                    </div>
                                </div>
                            </div>

                            <!-- Quality Levels Visual -->
                            <div class="stat-card mt-2">
                                <div class="stat-header">
                                    <i class="bi bi-sliders"></i>
                                    <span>Quality Levels</span>
                                    <span id="abrModeIndicator" class="badge bg-success ms-auto">Auto ABR</span>
                                </div>
                                <div id="qualityLevelsList" class="quality-levels-list">
                                    <!-- Populated dynamically -->
                                </div>
                            </div>
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
                                <span id="outputFormatAbrBadge" class="badge bg-warning text-dark ms-2 d-none">ABR</span>
                            </div>
                            <div class="card-body py-2">
                                <div class="row small" id="outputFormatStandard">
                                    <div class="col-6">
                                        <div class="mb-1"><span class="text-muted">Video:</span> <span id="outputVideoCodec">-</span></div>
                                        <div class="mb-1"><span class="text-muted">Resolution:</span> <span id="outputResolution">-</span></div>
                                    </div>
                                    <div class="col-6">
                                        <div class="mb-1"><span class="text-muted">Audio:</span> <span id="outputAudioCodec">-</span></div>
                                        <div><span class="text-muted">Channels:</span> <span id="outputAudioChannels">-</span></div>
                                    </div>
                                </div>
                                <div id="outputFormatAbr" class="d-none small">
                                    <div class="mb-1"><span class="text-muted">Audio:</span> <span id="outputAudioCodecAbr">-</span> (<span id="outputAudioChannelsAbr">-</span>)</div>
                                    <div class="text-muted mb-1">Video Streams:</div>
                                    <div id="outputVideoStreamsList"></div>
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
                                <span id="outputAbrBadge" class="badge bg-warning text-dark ms-2 d-none">ABR</span>
                            </div>
                            <div class="card-body py-2">
                                <div id="outputBitrateContainer">
                                    <div class="d-flex justify-content-between align-items-center mb-1">
                                        <span class="text-muted small">Video</span>
                                        <span class="fw-bold text-success" id="monitorOutputVideoBitrate">-</span>
                                    </div>
                                </div>
                                <div id="outputVideoPidsList"></div>
                                <div class="d-flex justify-content-between align-items-center">
                                    <span class="text-muted small">Audio</span>
                                    <span class="fw-bold text-success" id="monitorOutputAudioBitrate">-</span>
                                </div>
                                <hr class="my-1 d-none" id="outputTotalSeparator">
                                <div class="d-flex justify-content-between align-items-center d-none" id="outputTotalRow">
                                    <span class="text-muted small fw-bold">Total</span>
                                    <span class="fw-bold text-success" id="monitorOutputTotalBitrate">-</span>
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

                <!-- Stream Health -->
                <div class="card mb-3">
                    <div class="card-header py-2 d-flex justify-content-between align-items-center">
                        <strong><i class="bi bi-heart-pulse me-1"></i>Stream Health</strong>
                        <span id="healthStatus" class="badge bg-success">OK</span>
                    </div>
                    <div class="card-body py-2">
                        <div class="d-flex justify-content-between align-items-center">
                            <span class="text-muted small">Continuity Errors (recent)</span>
                            <span class="fw-bold" id="continuityErrorCount">0</span>
                        </div>
                        <div id="continuityErrorDetails" class="mt-2 small text-muted d-none">
                            <div class="fw-semibold">Errors by PID:</div>
                            <div id="continuityErrorsByPid"></div>
                        </div>
                        <div class="mt-2 small text-muted">
                            <i class="bi bi-info-circle me-1"></i>
                            Continuity errors indicate packet loss in the source stream
                        </div>
                    </div>
                </div>

                <!-- A/V Sync Monitor -->
                <div class="card">
                    <div class="card-header py-2 d-flex justify-content-between align-items-center">
                        <strong><i class="bi bi-soundwave me-1"></i>A/V Sync Monitor</strong>
                        <div class="small">
                            <span id="avsyncStatus" class="badge bg-secondary">Loading...</span>
                            Updated: <span id="avsyncLastUpdate">-</span>
                        </div>
                    </div>
                    <div class="card-body py-2">
                        <!-- A/V Sync Stats -->
                        <div class="row g-2 mb-3">
                            <div class="col-4">
                                <div class="avsync-stat-card">
                                    <div class="stat-label">Audio→Video</div>
                                    <div class="stat-value" id="avsyncA2V">-</div>
                                    <div class="stat-sublabel">A2V gap</div>
                                </div>
                            </div>
                            <div class="col-4">
                                <div class="avsync-stat-card">
                                    <div class="stat-label">Video→Audio</div>
                                    <div class="stat-value" id="avsyncV2A">-</div>
                                    <div class="stat-sublabel">V2A gap</div>
                                </div>
                            </div>
                            <div class="col-4">
                                <div class="avsync-stat-card">
                                    <div class="stat-label">Status</div>
                                    <div class="stat-value" id="avsyncCurrentStatus">-</div>
                                    <div class="stat-sublabel" id="avsyncSamples">- samples</div>
                                </div>
                            </div>
                        </div>
                        <!-- A/V Sync Graph -->
                        <div class="position-relative" style="height: 150px;">
                            <canvas id="avsyncChart"></canvas>
                        </div>
                        <div class="text-center mt-1">
                            <small class="text-muted">24-hour A/V sync history (polled every 5 minutes)</small>
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
let avsyncChart = null;
let avsyncUpdateInterval = null;
let inputVideoHistory = [];
let inputAudioHistory = [];
let outputVideoHistory = [];  // For non-ABR: single video stream
let outputAudioHistory = [];
let bitrateTimestamps = [];
let outputVideoPidsHistory = {};  // For ABR: per-PID video history {pid: [bitrates]}
let currentVideoPids = [];  // List of video PIDs for current ABR transcoder
let currentIsAbr = false;  // Whether current transcoder is ABR mode
const MAX_HISTORY_POINTS = 60;

// Color palette for ABR video PIDs
const VIDEO_PID_COLORS = [
    '#198754',  // Green
    '#0d6efd',  // Blue
    '#6f42c1',  // Purple
    '#d63384',  // Pink
    '#fd7e14',  // Orange
    '#20c997',  // Teal
];

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

    // Base datasets: Input Video, Input Audio, Output Audio
    // For non-ABR, we add a single Output Video dataset
    // For ABR, we add per-PID video datasets dynamically
    const datasets = [
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
    ];

    bitrateChart = new Chart(ctx, {
        type: 'line',
        data: {
            labels: [],
            datasets: datasets
        },
        options: {
            responsive: true,
            maintainAspectRatio: false,
            interaction: {
                intersect: false,
                mode: 'index'
            },
            plugins: {
                legend: { display: true, position: 'bottom', labels: { boxWidth: 12, padding: 10 } },
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

// Add video PID datasets to chart for ABR mode
function setupAbrChartDatasets(videoPids) {
    if (!bitrateChart) return;

    // Remove any existing output video datasets (indices 3+)
    while (bitrateChart.data.datasets.length > 3) {
        bitrateChart.data.datasets.pop();
    }

    // Add a dataset for each video PID
    videoPids.forEach((pid, idx) => {
        const color = VIDEO_PID_COLORS[idx % VIDEO_PID_COLORS.length];
        bitrateChart.data.datasets.push({
            label: `Video PID ${pid}`,
            data: [],
            borderColor: color,
            backgroundColor: color + '1A',  // 10% opacity
            fill: false,
            tension: 0.3,
            pointRadius: 0,
            borderWidth: 2
        });
    });

    bitrateChart.update('none');
}

// Add single video dataset for non-ABR mode
function setupStandardChartDatasets() {
    if (!bitrateChart) return;

    // Remove any existing output video datasets (indices 3+)
    while (bitrateChart.data.datasets.length > 3) {
        bitrateChart.data.datasets.pop();
    }

    // Add single output video dataset
    bitrateChart.data.datasets.push({
        label: 'Output Video',
        data: [],
        borderColor: '#198754',
        backgroundColor: 'rgba(25, 135, 84, 0.1)',
        fill: false,
        tension: 0.3,
        pointRadius: 0,
        borderWidth: 2
    });

    bitrateChart.update('none');
}

// Load historical bitrate data for output
async function loadBitrateHistory(transcoderId) {
    try {
        const response = await fetch(`api/transcoders.php?action=metrics_history&id=${transcoderId}`);
        const data = await response.json();

        if (data.success && data.pids) {
            const audioPid = data.audio_pid;
            const isAbr = data.is_abr && data.video_pids && data.video_pids.length > 1;

            // Store ABR state
            currentIsAbr = isAbr;
            currentVideoPids = isAbr ? data.video_pids : [data.video_pid];

            // Setup chart datasets based on ABR mode
            if (isAbr) {
                setupAbrChartDatasets(currentVideoPids);
            } else {
                setupStandardChartDatasets();
            }

            // Build combined timeline from all PIDs
            const timelineMap = new Map();

            // Add video data for all video PIDs
            for (const pid of currentVideoPids) {
                if (data.pids[pid] && data.pids[pid].history) {
                    for (const [ts, bitrate] of data.pids[pid].history) {
                        if (!timelineMap.has(ts)) {
                            timelineMap.set(ts, { videos: {}, audio: null });
                        }
                        timelineMap.get(ts).videos[pid] = bitrate;
                    }
                }
            }

            // Add audio data
            if (data.pids[audioPid] && data.pids[audioPid].history) {
                for (const [ts, bitrate] of data.pids[audioPid].history) {
                    if (!timelineMap.has(ts)) {
                        timelineMap.set(ts, { videos: {}, audio: null });
                    }
                    timelineMap.get(ts).audio = bitrate;
                }
            }

            // Sort by timestamp
            const sortedTimestamps = Array.from(timelineMap.keys()).sort((a, b) => a - b);

            // Reset arrays
            outputVideoHistory = [];
            outputAudioHistory = [];
            bitrateTimestamps = [];
            outputVideoPidsHistory = {};
            for (const pid of currentVideoPids) {
                outputVideoPidsHistory[pid] = [];
            }

            // Carry forward last known values for missing data
            let lastVideos = {};
            for (const pid of currentVideoPids) {
                lastVideos[pid] = 0;
            }
            let lastAudio = 0;

            for (const ts of sortedTimestamps) {
                const values = timelineMap.get(ts);

                // Update last known values
                for (const pid of currentVideoPids) {
                    if (values.videos[pid] !== undefined) {
                        lastVideos[pid] = values.videos[pid];
                    }
                }
                if (values.audio !== null) lastAudio = values.audio;

                // Check if we have any data
                const hasAnyVideo = Object.values(lastVideos).some(v => v > 0);
                if (hasAnyVideo || lastAudio > 0) {
                    for (const pid of currentVideoPids) {
                        outputVideoPidsHistory[pid].push(lastVideos[pid]);
                    }
                    // For non-ABR compatibility, sum all video bitrates
                    outputVideoHistory.push(Object.values(lastVideos).reduce((a, b) => a + b, 0));
                    outputAudioHistory.push(lastAudio);
                    bitrateTimestamps.push(ts);
                }
            }

            // Limit to last MAX_HISTORY_POINTS for display
            if (bitrateTimestamps.length > MAX_HISTORY_POINTS) {
                const start = bitrateTimestamps.length - MAX_HISTORY_POINTS;
                outputVideoHistory = outputVideoHistory.slice(start);
                outputAudioHistory = outputAudioHistory.slice(start);
                bitrateTimestamps = bitrateTimestamps.slice(start);
                for (const pid of currentVideoPids) {
                    outputVideoPidsHistory[pid] = outputVideoPidsHistory[pid].slice(start);
                }
            }

            // Update chart with loaded history
            if (bitrateChart && bitrateTimestamps.length > 0) {
                const labels = bitrateTimestamps.map(ts => {
                    return new Date(ts * 1000).toLocaleTimeString([], {hour: '2-digit', minute: '2-digit', second: '2-digit'});
                });

                bitrateChart.data.labels = labels;
                // Dataset 2 is Output Audio (indices 0, 1, 2 are Input Video, Input Audio, Output Audio)
                bitrateChart.data.datasets[2].data = [...outputAudioHistory];

                // Datasets 3+ are video PIDs
                if (isAbr) {
                    currentVideoPids.forEach((pid, idx) => {
                        if (bitrateChart.data.datasets[3 + idx]) {
                            bitrateChart.data.datasets[3 + idx].data = [...outputVideoPidsHistory[pid]];
                        }
                    });
                } else {
                    if (bitrateChart.data.datasets[3]) {
                        bitrateChart.data.datasets[3].data = [...outputVideoHistory];
                    }
                }
                bitrateChart.update('none');
            }
        }
    } catch (e) {
        console.error('Failed to load bitrate history:', e);
    }
}

// Load historical bitrate data for input (from linked source service)
async function loadInputBitrateHistory(inputId) {
    if (!inputId) return;

    try {
        const response = await fetch(`api/inputs.php?action=metrics_history&id=${inputId}`);
        const data = await response.json();

        if (!data.success || !data.pids) {
            console.log('No input history available for:', inputId);
            return;
        }

        // Determine video and audio PIDs (video has higher bitrate)
        let videoPid = null, audioPids = [];
        for (const [pid, pidData] of Object.entries(data.pids)) {
            if (pidData.history && pidData.history.length > 0) {
                const lastSamples = pidData.history.slice(-5);
                const avgBitrate = lastSamples.reduce((a, b) => a + b[1], 0) / lastSamples.length;
                if (avgBitrate > 500000 && !videoPid) {
                    videoPid = pid;
                } else {
                    audioPids.push(pid);
                }
            }
        }

        if (!videoPid && audioPids.length === 0) {
            return;
        }

        // Build combined timeline from all PIDs
        const timelineMap = new Map();

        // Add video data
        if (videoPid && data.pids[videoPid].history) {
            for (const [ts, bitrate] of data.pids[videoPid].history) {
                if (!timelineMap.has(ts)) {
                    timelineMap.set(ts, { video: 0, audio: 0 });
                }
                timelineMap.get(ts).video = bitrate;
            }
        }

        // Add audio data (sum all audio PIDs)
        for (const audioPid of audioPids) {
            if (data.pids[audioPid] && data.pids[audioPid].history) {
                for (const [ts, bitrate] of data.pids[audioPid].history) {
                    if (!timelineMap.has(ts)) {
                        timelineMap.set(ts, { video: 0, audio: 0 });
                    }
                    timelineMap.get(ts).audio += bitrate;
                }
            }
        }

        // Sort by timestamp
        const sortedTimestamps = Array.from(timelineMap.keys()).sort((a, b) => a - b);

        // We need to merge input timestamps with existing bitrateTimestamps from output history
        // Create a unified timeline with both input and output data
        const unifiedTimeline = new Map();

        // Add existing output data to unified timeline
        for (let i = 0; i < bitrateTimestamps.length; i++) {
            const ts = bitrateTimestamps[i];
            unifiedTimeline.set(ts, {
                inVideo: 0,
                inAudio: 0,
                outVideo: outputVideoHistory[i] || 0,
                outAudio: outputAudioHistory[i] || 0
            });
        }

        // Add input data to unified timeline
        for (const ts of sortedTimestamps) {
            const values = timelineMap.get(ts);
            if (!unifiedTimeline.has(ts)) {
                unifiedTimeline.set(ts, {
                    inVideo: 0,
                    inAudio: 0,
                    outVideo: 0,
                    outAudio: 0
                });
            }
            unifiedTimeline.get(ts).inVideo = values.video;
            unifiedTimeline.get(ts).inAudio = values.audio;
        }

        // Sort unified timeline and rebuild all arrays
        const unifiedSorted = Array.from(unifiedTimeline.keys()).sort((a, b) => a - b);

        // Reset arrays
        inputVideoHistory = [];
        inputAudioHistory = [];
        outputVideoHistory = [];
        outputAudioHistory = [];
        bitrateTimestamps = [];

        // Carry forward last known values
        let lastInVid = 0, lastInAud = 0, lastOutVid = 0, lastOutAud = 0;

        for (const ts of unifiedSorted) {
            const v = unifiedTimeline.get(ts);
            if (v.inVideo > 0) lastInVid = v.inVideo;
            if (v.inAudio > 0) lastInAud = v.inAudio;
            if (v.outVideo > 0) lastOutVid = v.outVideo;
            if (v.outAudio > 0) lastOutAud = v.outAudio;

            // Only include if we have any data
            if (lastInVid > 0 || lastOutVid > 0) {
                inputVideoHistory.push(lastInVid);
                inputAudioHistory.push(lastInAud);
                outputVideoHistory.push(lastOutVid);
                outputAudioHistory.push(lastOutAud);
                bitrateTimestamps.push(ts);
            }
        }

        // Limit to last MAX_HISTORY_POINTS
        if (bitrateTimestamps.length > MAX_HISTORY_POINTS) {
            const start = bitrateTimestamps.length - MAX_HISTORY_POINTS;
            inputVideoHistory = inputVideoHistory.slice(start);
            inputAudioHistory = inputAudioHistory.slice(start);
            outputVideoHistory = outputVideoHistory.slice(start);
            outputAudioHistory = outputAudioHistory.slice(start);
            bitrateTimestamps = bitrateTimestamps.slice(start);
        }

        // Update chart
        if (bitrateChart && bitrateTimestamps.length > 0) {
            const labels = bitrateTimestamps.map(ts => {
                return new Date(ts * 1000).toLocaleTimeString([], {hour: '2-digit', minute: '2-digit', second: '2-digit'});
            });

            bitrateChart.data.labels = labels;
            bitrateChart.data.datasets[0].data = [...inputVideoHistory];
            bitrateChart.data.datasets[1].data = [...inputAudioHistory];
            // Dataset 2 is now Output Audio
            bitrateChart.data.datasets[2].data = [...outputAudioHistory];
            // Dataset 3+ are output video PIDs
            if (currentIsAbr) {
                // For ABR, we need to update per-PID data
                currentVideoPids.forEach((pid, idx) => {
                    if (bitrateChart.data.datasets[3 + idx] && outputVideoPidsHistory[pid]) {
                        bitrateChart.data.datasets[3 + idx].data = [...outputVideoPidsHistory[pid]];
                    }
                });
            } else {
                // For non-ABR, dataset 3 is the single output video
                if (bitrateChart.data.datasets[3]) {
                    bitrateChart.data.datasets[3].data = [...outputVideoHistory];
                }
            }
            bitrateChart.update('none');
        }
    } catch (e) {
        console.error('Failed to load input bitrate history:', e);
    }
}

// Initialize A/V sync chart
function initAVSyncChart() {
    const ctx = document.getElementById('avsyncChart').getContext('2d');

    if (avsyncChart) {
        avsyncChart.destroy();
    }

    avsyncChart = new Chart(ctx, {
        type: 'line',
        data: {
            labels: [],
            datasets: [
                {
                    label: 'A→V',
                    data: [],
                    borderColor: '#0d6efd',
                    backgroundColor: 'rgba(13, 110, 253, 0.1)',
                    fill: false,
                    tension: 0.3,
                    pointRadius: 1,
                    borderWidth: 2
                },
                {
                    label: 'V→A',
                    data: [],
                    borderColor: '#6c757d',
                    backgroundColor: 'rgba(108, 117, 125, 0.1)',
                    fill: false,
                    tension: 0.3,
                    pointRadius: 1,
                    borderWidth: 2
                }
            ]
        },
        options: {
            responsive: true,
            maintainAspectRatio: false,
            plugins: {
                legend: { display: true, position: 'bottom', labels: { boxWidth: 12, font: { size: 10 } } }
            },
            scales: {
                x: { display: true, ticks: { font: { size: 9 }, maxTicksLimit: 8 } },
                y: {
                    display: true,
                    beginAtZero: true,
                    title: { display: false },
                    ticks: { font: { size: 9 }, callback: v => v + 'ms' }
                }
            }
        }
    });
}

// Load A/V sync data from cari-avsync service
async function loadAVSyncHistory(transcoderId) {
    try {
        document.getElementById('avsyncStatus').className = 'badge bg-secondary';
        document.getElementById('avsyncStatus').textContent = 'Loading...';

        // Fetch from cari-avsync API
        const response = await fetch(`http://${window.location.hostname}:8082/history/${transcoderId}`);

        if (!response.ok) {
            throw new Error(`HTTP ${response.status}`);
        }

        const data = await response.json();

        if (!data.running) {
            document.getElementById('avsyncStatus').className = 'badge bg-warning';
            document.getElementById('avsyncStatus').textContent = 'Not Running';
            document.getElementById('avsyncA2V').textContent = '-';
            document.getElementById('avsyncV2A').textContent = '-';
            document.getElementById('avsyncCurrentStatus').textContent = '-';
            document.getElementById('avsyncSamples').textContent = 'Transcoder not running';
            return;
        }

        // Determine which data to show in stat cards
        // Prefer current, fall back to last history entry
        let displayData = null;
        if (data.current && data.current.timestamp && data.current.timestamp.length > 0) {
            displayData = data.current;
        } else if (data.history && data.history.length > 0) {
            displayData = data.history[data.history.length - 1];
        }

        // Update stat cards
        if (displayData) {
            const a2v = displayData.a2v_mean_ms;
            const v2a = displayData.v2a_mean_ms;
            const status = displayData.status;
            const samples = displayData.a2v_samples || 0;

            document.getElementById('avsyncA2V').textContent = a2v.toFixed(1) + ' ms';
            document.getElementById('avsyncV2A').textContent = v2a.toFixed(1) + ' ms';

            const statusEl = document.getElementById('avsyncCurrentStatus');
            statusEl.textContent = status;
            statusEl.className = 'stat-value status-' + status.toLowerCase();

            document.getElementById('avsyncSamples').textContent = samples + ' samples';

            // Use unix_ts for browser-local time display
            if (displayData.unix_ts) {
                const localTime = new Date(displayData.unix_ts * 1000).toLocaleTimeString();
                document.getElementById('avsyncLastUpdate').textContent = localTime;
            } else {
                document.getElementById('avsyncLastUpdate').textContent = displayData.timestamp;
            }

            // Update status badge
            const badge = document.getElementById('avsyncStatus');
            if (status === 'OK') {
                badge.className = 'badge avsync-ok';
                badge.textContent = 'OK';
            } else if (status === 'WARNING') {
                badge.className = 'badge avsync-warning';
                badge.textContent = 'Warning';
            } else {
                badge.className = 'badge avsync-error';
                badge.textContent = 'Error';
            }
        } else {
            document.getElementById('avsyncStatus').className = 'badge bg-secondary';
            document.getElementById('avsyncStatus').textContent = 'No Data';
            document.getElementById('avsyncA2V').textContent = '-';
            document.getElementById('avsyncV2A').textContent = '-';
            document.getElementById('avsyncCurrentStatus').textContent = '-';
            document.getElementById('avsyncSamples').textContent = 'No measurements yet';
        }

        // Update chart with history
        if (avsyncChart && data.history && data.history.length > 0) {
            avsyncChart.data.labels = [];
            avsyncChart.data.datasets[0].data = [];
            avsyncChart.data.datasets[1].data = [];

            for (const entry of data.history) {
                let shortTime = '';
                if (entry.unix_ts) {
                    shortTime = new Date(entry.unix_ts * 1000).toLocaleTimeString([], {hour: '2-digit', minute:'2-digit'});
                } else if (entry.timestamp) {
                    shortTime = entry.timestamp.split(' ')[1] || entry.timestamp;
                }
                avsyncChart.data.labels.push(shortTime);
                avsyncChart.data.datasets[0].data.push(entry.a2v_mean_ms);
                avsyncChart.data.datasets[1].data.push(entry.v2a_mean_ms);
            }
            avsyncChart.update();
        }
    } catch (e) {
        console.error('Failed to load A/V sync history:', e);
        document.getElementById('avsyncStatus').className = 'badge bg-danger';
        document.getElementById('avsyncStatus').textContent = 'Error';
        document.getElementById('avsyncA2V').textContent = '-';
        document.getElementById('avsyncV2A').textContent = '-';
        document.getElementById('avsyncCurrentStatus').textContent = 'Unavailable';
        document.getElementById('avsyncCurrentStatus').className = 'stat-value';
        document.getElementById('avsyncSamples').textContent = 'Service not reachable';
    }
}

// Load full continuity error count from log file
async function loadContinuityErrors(transcoderId) {
    try {
        const response = await fetch(`api/transcoders.php?action=continuity_errors&id=${transcoderId}`);
        const data = await response.json();

        if (!data.success) {
            console.log('Failed to load continuity errors:', data.error);
            return;
        }

        const errorCount = data.total_errors || 0;
        const errorsByPid = data.errors_by_pid || {};

        document.getElementById('continuityErrorCount').textContent = errorCount;

        // Update health status badge
        const healthBadge = document.getElementById('healthStatus');
        if (errorCount === 0) {
            healthBadge.className = 'badge bg-success';
            healthBadge.textContent = 'OK';
        } else if (errorCount < 50) {
            healthBadge.className = 'badge bg-warning';
            healthBadge.textContent = 'Warning';
        } else {
            healthBadge.className = 'badge bg-danger';
            healthBadge.textContent = 'Errors';
        }

        // Show error details by PID if there are errors
        const detailsDiv = document.getElementById('continuityErrorDetails');
        const pidDiv = document.getElementById('continuityErrorsByPid');
        if (errorCount > 0 && Object.keys(errorsByPid).length > 0) {
            let pidHtml = '';
            for (const [pid, count] of Object.entries(errorsByPid)) {
                pidHtml += `<span class="me-2">PID ${pid}: <strong>${count}</strong></span>`;
            }
            pidDiv.innerHTML = pidHtml;
            detailsDiv.classList.remove('d-none');
        } else {
            detailsDiv.classList.add('d-none');
        }
    } catch (e) {
        console.error('Failed to load continuity errors:', e);
    }
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
                // Load detailed input format info and history (only once per modal open)
                if (!window.inputFormatLoaded) {
                    window.inputFormatLoaded = true;
                    window.currentSourceService = metrics.source_service;
                    loadInputMediaInfo(metrics.source_service);
                    // Load input bitrate history after output history
                    loadInputBitrateHistory(metrics.source_service);
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
            document.getElementById('monitorOutputAudioBitrate').textContent = formatBitrate(metrics.output_audio_bitrate || 0);

            // Handle ABR mode with multiple video PIDs
            const isAbr = metrics.is_abr && metrics.video_bitrates_by_pid && Object.keys(metrics.video_bitrates_by_pid).length > 1;
            window.currentIsAbr = isAbr;

            if (isAbr) {
                // Show ABR badge
                document.getElementById('outputAbrBadge').classList.remove('d-none');

                // Hide single video row, show per-PID rows
                document.getElementById('outputBitrateContainer').classList.add('d-none');

                // Build per-PID bitrate list
                const pidsList = document.getElementById('outputVideoPidsList');
                pidsList.innerHTML = '';
                let totalVideoBitrate = 0;

                for (const [pid, bitrate] of Object.entries(metrics.video_bitrates_by_pid)) {
                    totalVideoBitrate += bitrate;
                    const row = document.createElement('div');
                    row.className = 'd-flex justify-content-between align-items-center mb-1';
                    row.innerHTML = `<span class="text-muted small">Video PID ${pid}</span><span class="fw-bold text-success">${formatBitrate(bitrate)}</span>`;
                    pidsList.appendChild(row);
                }

                // Show total row
                document.getElementById('outputTotalSeparator').classList.remove('d-none');
                document.getElementById('outputTotalRow').classList.remove('d-none');
                document.getElementById('monitorOutputTotalBitrate').textContent = formatBitrate(metrics.output_total_bitrate || 0);
            } else {
                // Standard mode - single video
                document.getElementById('outputAbrBadge').classList.add('d-none');
                document.getElementById('outputBitrateContainer').classList.remove('d-none');
                document.getElementById('outputVideoPidsList').innerHTML = '';
                document.getElementById('outputTotalSeparator').classList.add('d-none');
                document.getElementById('outputTotalRow').classList.add('d-none');
                document.getElementById('monitorOutputVideoBitrate').textContent = formatBitrate(metrics.output_video_bitrate || 0);
            }

            // Add to history
            inputVideoHistory.push(metrics.input_video_bitrate || 0);
            inputAudioHistory.push(metrics.input_audio_bitrate || 0);
            outputAudioHistory.push(metrics.output_audio_bitrate || 0);
            bitrateTimestamps.push(Math.floor(Date.now() / 1000));

            // For ABR mode, update per-PID video history
            if (currentIsAbr && metrics.video_bitrates_by_pid) {
                for (const pid of currentVideoPids) {
                    if (!outputVideoPidsHistory[pid]) {
                        outputVideoPidsHistory[pid] = [];
                    }
                    outputVideoPidsHistory[pid].push(metrics.video_bitrates_by_pid[pid] || 0);
                }
            }
            // Always update combined video history for compatibility
            outputVideoHistory.push(metrics.output_video_bitrate || 0);

            if (inputVideoHistory.length > MAX_HISTORY_POINTS) {
                inputVideoHistory.shift();
                inputAudioHistory.shift();
                outputVideoHistory.shift();
                outputAudioHistory.shift();
                bitrateTimestamps.shift();
                if (currentIsAbr) {
                    for (const pid of currentVideoPids) {
                        if (outputVideoPidsHistory[pid]) {
                            outputVideoPidsHistory[pid].shift();
                        }
                    }
                }
            }

            // Update chart
            if (bitrateChart) {
                // Generate time labels from timestamps
                const labels = bitrateTimestamps.map(ts => {
                    return new Date(ts * 1000).toLocaleTimeString([], {hour: '2-digit', minute: '2-digit', second: '2-digit'});
                });

                bitrateChart.data.labels = labels;
                bitrateChart.data.datasets[0].data = [...inputVideoHistory];
                bitrateChart.data.datasets[1].data = [...inputAudioHistory];
                // Dataset 2 is Output Audio
                bitrateChart.data.datasets[2].data = [...outputAudioHistory];
                // Dataset 3+ are output video PIDs
                if (currentIsAbr) {
                    currentVideoPids.forEach((pid, idx) => {
                        if (bitrateChart.data.datasets[3 + idx] && outputVideoPidsHistory[pid]) {
                            bitrateChart.data.datasets[3 + idx].data = [...outputVideoPidsHistory[pid]];
                        }
                    });
                } else {
                    if (bitrateChart.data.datasets[3]) {
                        bitrateChart.data.datasets[3].data = [...outputVideoHistory];
                    }
                }
                bitrateChart.update('none');
            }

            // Update timestamp
            document.getElementById('graphLastUpdate').textContent = new Date().toLocaleTimeString();
            document.getElementById('graphStatus').className = 'badge bg-success';
            document.getElementById('graphStatus').textContent = 'Live';

            // Update continuity error display
            const errorCount = metrics.continuity_errors || 0;
            const errorsByPid = metrics.continuity_errors_by_pid || {};
            document.getElementById('continuityErrorCount').textContent = errorCount;

            // Update health status badge
            const healthBadge = document.getElementById('healthStatus');
            if (errorCount === 0) {
                healthBadge.className = 'badge bg-success';
                healthBadge.textContent = 'OK';
            } else if (errorCount < 10) {
                healthBadge.className = 'badge bg-warning';
                healthBadge.textContent = 'Warning';
            } else {
                healthBadge.className = 'badge bg-danger';
                healthBadge.textContent = 'Errors';
            }

            // Show error details by PID if there are errors
            const detailsDiv = document.getElementById('continuityErrorDetails');
            const pidDiv = document.getElementById('continuityErrorsByPid');
            if (errorCount > 0 && Object.keys(errorsByPid).length > 0) {
                let pidHtml = '';
                for (const [pid, count] of Object.entries(errorsByPid)) {
                    pidHtml += `<span class="me-2">PID ${pid}: <strong>${count}</strong></span>`;
                }
                pidDiv.innerHTML = pidHtml;
                detailsDiv.classList.remove('d-none');
            } else {
                detailsDiv.classList.add('d-none');
            }
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
            document.getElementById('outputAudioChannels').textContent = 'N/A';
            return;
        }

        // Check if we have multiple video streams (ABR mode)
        const videoStreams = data.videos || (data.video ? [data.video] : []);
        const isAbr = videoStreams.length > 1;

        if (isAbr) {
            // Show ABR format display
            document.getElementById('outputFormatAbrBadge').classList.remove('d-none');
            document.getElementById('outputFormatStandard').classList.add('d-none');
            document.getElementById('outputFormatAbr').classList.remove('d-none');

            // Update audio info
            if (data.audio && data.audio.length > 0) {
                const track = data.audio[0];
                document.getElementById('outputAudioCodecAbr').textContent =
                    (track.codec || '-').toUpperCase() + (track.profile ? ` ${track.profile}` : '');
                document.getElementById('outputAudioChannelsAbr').textContent =
                    track.channels ? `${track.channels} ch` : '-';
            }

            // Build video streams list
            const videosList = document.getElementById('outputVideoStreamsList');
            videosList.innerHTML = '';
            for (const video of videoStreams) {
                const row = document.createElement('div');
                row.className = 'ms-2 mb-1';
                const codec = (video.codec || '-').toUpperCase();
                const resolution = video.width && video.height ? `${video.width}x${video.height}` : '-';
                const pid = video.pid ? ` (PID ${video.pid})` : '';
                row.innerHTML = `<small>${codec} ${resolution}${pid}</small>`;
                videosList.appendChild(row);
            }
        } else {
            // Standard single-video format display
            document.getElementById('outputFormatAbrBadge').classList.add('d-none');
            document.getElementById('outputFormatStandard').classList.remove('d-none');
            document.getElementById('outputFormatAbr').classList.add('d-none');

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
                document.getElementById('outputAudioChannels').textContent =
                    track.channels ? `${track.channels} ch` : '-';
            }
        }
    } catch (e) {
        console.error('Failed to load output media info:', e);
        document.getElementById('outputVideoCodec').textContent = 'Error';
    }
}

// Show preview modal
async function showPreview(id, name) {
    document.getElementById('previewId').value = id;
    document.getElementById('previewName').textContent = name;
    document.getElementById('previewApiPort').value = '';

    // Reset format loaded flags
    window.inputFormatLoaded = false;
    window.outputFormatLoaded = false;
    window.currentOutputAddress = null;
    window.previewStarted = false;
    window.variantCount = 1;
    window.variantBitrates = [];

    // Reset player state
    if (outputHlsPlayer) {
        outputHlsPlayer.destroy();
        outputHlsPlayer = null;
    }
    outputPlayerRunning = false;
    ccEnabled = false;
    document.getElementById('outputVideo').classList.add('d-none');
    document.getElementById('videoLoadingOverlay').classList.remove('d-none');
    document.getElementById('startPlayerBtn').classList.remove('d-none');
    document.getElementById('stopPlayerBtn').classList.add('d-none');
    document.getElementById('playerStatus').className = 'badge bg-secondary';
    document.getElementById('playerStatus').textContent = 'Ready';
    document.getElementById('videoStatusText').textContent = 'Starting preview in background...';

    // Reset quality selector and CC button
    const qualitySelector = document.getElementById('qualitySelector');
    qualitySelector.innerHTML = '<option value="-1">Auto</option>';
    qualitySelector.classList.add('d-none');
    document.getElementById('ccBtn').classList.add('d-none');
    document.getElementById('ccBtn').classList.remove('btn-primary');
    document.getElementById('ccBtn').classList.add('btn-outline-secondary');

    // Reset history (all series + timestamps)
    inputVideoHistory = [];
    inputAudioHistory = [];
    outputVideoHistory = [];
    outputAudioHistory = [];
    bitrateTimestamps = [];
    outputVideoPidsHistory = {};
    currentVideoPids = [];
    currentIsAbr = false;

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
    document.getElementById('outputAudioChannels').textContent = '-';

    // Reset bitrate displays
    document.getElementById('monitorInputVideoBitrate').textContent = '-';
    document.getElementById('monitorInputAudioBitrate').textContent = '-';
    document.getElementById('monitorOutputVideoBitrate').textContent = '-';
    document.getElementById('monitorOutputAudioBitrate').textContent = '-';

    // Reset health/continuity error displays
    document.getElementById('healthStatus').className = 'badge bg-secondary';
    document.getElementById('healthStatus').textContent = 'Loading...';
    document.getElementById('continuityErrorCount').textContent = '-';
    document.getElementById('continuityErrorDetails').classList.add('d-none');

    // Initialize charts
    initBitrateChart();
    initAVSyncChart();

    // Load historical output bitrate data first (await to ensure it's ready before input history)
    await loadBitrateHistory(id);

    // Load initial metrics - wait for it to get output_address and start preview
    await loadPreviewMetricsAndStartPreview();

    // Load A/V sync data and full continuity error count
    loadAVSyncHistory(id);
    loadContinuityErrors(id);

    // Start polling
    if (previewInterval) clearInterval(previewInterval);
    previewInterval = setInterval(loadPreviewMetrics, 5000);

    // Start A/V sync updates (every 5 minutes)
    if (avsyncUpdateInterval) clearInterval(avsyncUpdateInterval);
    avsyncUpdateInterval = setInterval(() => loadAVSyncHistory(id), 300000);

    // Show modal
    if (!previewModal) {
        previewModal = new bootstrap.Modal(document.getElementById('previewModal'));
    }
    previewModal.show();
}

// Load metrics and start preview immediately (called once when modal opens)
async function loadPreviewMetricsAndStartPreview() {
    const id = document.getElementById('previewId').value;
    if (!id) return;

    try {
        const response = await fetch(`api/transcoders.php?action=all_metrics`);
        const data = await response.json();

        if (data.success && data.transcoders && data.transcoders[id]) {
            const metrics = data.transcoders[id];

            // Store output address
            if (metrics.output_address) {
                window.currentOutputAddress = metrics.output_address;
            }

            // Store api_port
            if (metrics.api_port) {
                document.getElementById('previewApiPort').value = metrics.api_port;
            }

            // Store ABR variant info
            if (metrics.is_abr && metrics.variant_count > 1) {
                window.variantCount = metrics.variant_count;
                window.variantBitrates = metrics.variant_bitrates || [];
            }

            // Start preview in background if we have output address
            if (window.currentOutputAddress && !window.previewStarted) {
                await startPreviewInBackground();
            }
        }
    } catch (e) {
        console.error('Failed to load metrics for preview:', e);
    }
}

// Start preview in background (called when modal opens)
async function startPreviewInBackground() {
    const id = document.getElementById('previewId').value;
    const apiPort = document.getElementById('previewApiPort').value;
    const outputAddress = window.currentOutputAddress;

    if (!outputAddress || !apiPort) {
        return;
    }

    const folderName = `transcoder-${id}`;
    const outputDir = `/var/www/caritrans/public/preview/${folderName}`;
    const previewPort = parseInt(apiPort) + 100;

    try {
        // Check if preview is already running
        try {
            const statusResponse = await fetch(`http://${window.location.hostname}:${previewPort}/status`);
            if (statusResponse.ok) {
                // Preview already running, just set up keepalive
                window.previewStarted = true;
                currentPreviewPort = previewPort;
                document.getElementById('videoStatusText').textContent = 'Preview ready - click Start to play';

                // Start keepalive
                if (outputKeepaliveInterval) clearInterval(outputKeepaliveInterval);
                outputKeepaliveInterval = setInterval(sendOutputKeepalive, 30000);
                return;
            }
        } catch (e) {
            // Preview not running, continue to start it
        }

        // Build request with variants and bitrates
        const requestBody = {
            id: id,
            input_address: outputAddress,
            output_dir: outputDir,
            api_port: previewPort,
            variants: window.variantCount || 1,
            bitrates: window.variantBitrates || []
        };

        const response = await fetch('api/transcoders.php?action=start_preview', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(requestBody)
        });

        const result = await response.json();

        if (result.success) {
            window.previewStarted = true;
            currentPreviewPort = previewPort;
            document.getElementById('videoStatusText').textContent = 'Buffering in background...';

            // Start keepalive immediately
            if (outputKeepaliveInterval) clearInterval(outputKeepaliveInterval);
            outputKeepaliveInterval = setInterval(sendOutputKeepalive, 30000);
        } else {
            console.error('Failed to start preview:', result.error);
            document.getElementById('videoStatusText').textContent = 'Click Start to preview output';
        }
    } catch (e) {
        console.error('Failed to start preview in background:', e);
        document.getElementById('videoStatusText').textContent = 'Click Start to preview output';
    }
}

// Cleanup on modal close
document.getElementById('previewModal').addEventListener('hidden.bs.modal', function() {
    if (previewInterval) {
        clearInterval(previewInterval);
        previewInterval = null;
    }

    if (avsyncUpdateInterval) {
        clearInterval(avsyncUpdateInterval);
        avsyncUpdateInterval = null;
    }

    // Stop keepalive
    if (outputKeepaliveInterval) {
        clearInterval(outputKeepaliveInterval);
        outputKeepaliveInterval = null;
    }

    // Stop player if running
    if (outputPlayerRunning) {
        stopOutputPlayer();
    } else if (window.previewStarted && currentPreviewPort) {
        // Stop background preview even if player wasn't started
        fetch(`api/transcoders.php?action=stop_preview&api_port=${currentPreviewPort}`, {
            method: 'POST'
        }).catch(e => console.error('Failed to stop preview:', e));
    }

    // Reset state
    window.previewStarted = false;
    currentPreviewPort = null;

    inputVideoHistory = [];
    inputAudioHistory = [];
    outputVideoHistory = [];
    outputAudioHistory = [];
    bitrateTimestamps = [];
    outputVideoPidsHistory = {};
    currentVideoPids = [];
    currentIsAbr = false;
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
let outputKeepaliveInterval = null;
let currentPreviewPort = null;
let ccEnabled = false;

// Send keepalive to preview via API
async function sendOutputKeepalive() {
    if (!currentPreviewPort) return;
    try {
        await fetch(`api/transcoders.php?action=preview_keepalive&api_port=${currentPreviewPort}`, { method: 'POST' });
    } catch (e) {
        console.error('Keepalive failed:', e);
    }
}

// Wait for playlist to be ready by polling player_preview status
async function waitForPlaylistReady(previewPort, playlistUrl, maxAttempts = 30) {
    const statusText = document.getElementById('videoStatusText');

    for (let attempt = 0; attempt < maxAttempts; attempt++) {
        if (!outputPlayerRunning) {
            // Player was stopped while waiting
            return;
        }

        try {
            const response = await fetch(`http://${window.location.hostname}:${previewPort}/status`);
            if (response.ok) {
                const status = await response.json();
                statusText.textContent = `Buffering... (${status.segments || 0} segments)`;

                if (status.ready) {
                    // Playlist is ready, start the player
                    initOutputHlsPlayer(playlistUrl);
                    return;
                }
            }
        } catch (e) {
            // Preview server not responding yet, keep trying
            statusText.textContent = `Starting preview... (${attempt + 1}s)`;
        }

        // Wait 1 second before next check
        await new Promise(resolve => setTimeout(resolve, 1000));
    }

    // Timeout - try to play anyway
    console.warn('Playlist ready timeout, attempting to play anyway');
    initOutputHlsPlayer(playlistUrl);
}

// Start output player preview (connects to already-running preview)
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
    document.getElementById('playerStatus').textContent = 'Connecting...';
    document.getElementById('videoStatusText').textContent = 'Waiting for segments...';

    try {
        const folderName = `transcoder-${id}`;
        const previewPort = parseInt(apiPort) + 100;

        // If preview wasn't started in background, start it now
        if (!window.previewStarted) {
            await startPreviewInBackground();
        }

        // Mark player as running
        outputPlayerRunning = true;

        // Update status
        document.getElementById('playerStatus').className = 'badge bg-info';
        document.getElementById('playerStatus').textContent = 'Loading...';

        // Wait for playlist to be ready and connect
        const playlistUrl = `/preview/${folderName}/playlist.m3u8`;
        await waitForPlaylistReady(previewPort, playlistUrl);
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
    // Mark player as not running immediately to prevent race conditions
    outputPlayerRunning = false;

    const id = document.getElementById('previewId').value;
    const apiPort = document.getElementById('previewApiPort').value;
    const previewPort = parseInt(apiPort) + 100;

    // Stop keepalive
    if (outputKeepaliveInterval) {
        clearInterval(outputKeepaliveInterval);
        outputKeepaliveInterval = null;
    }
    currentPreviewPort = null;

    // Destroy HLS player
    if (outputHlsPlayer) {
        outputHlsPlayer.destroy();
        outputHlsPlayer = null;
    }

    const video = document.getElementById('outputVideo');
    video.pause();
    video.src = '';
    video.classList.add('d-none');
    document.getElementById('videoLoadingOverlay').classList.remove('d-none');

    try {
        await fetch(`api/transcoders.php?action=stop_preview&api_port=${previewPort}`, {
            method: 'POST'
        });
    } catch (e) {
        console.error('Failed to stop preview:', e);
    }

    // Reset state
    window.previewStarted = false;
    ccEnabled = false;
    document.getElementById('startPlayerBtn').classList.remove('d-none');
    document.getElementById('stopPlayerBtn').classList.add('d-none');
    document.getElementById('playerStatus').className = 'badge bg-secondary';
    document.getElementById('playerStatus').textContent = 'Stopped';
    document.getElementById('videoStatusText').textContent = 'Click Start to preview output';

    // Reset quality selector and CC button
    const qualitySelector = document.getElementById('qualitySelector');
    qualitySelector.innerHTML = '<option value="-1">Auto</option>';
    qualitySelector.classList.add('d-none');
    document.getElementById('ccBtn').classList.add('d-none');
    document.getElementById('ccBtn').classList.remove('btn-primary');
    document.getElementById('ccBtn').classList.add('btn-outline-secondary');

    // Reset and hide stats panel
    document.getElementById('statsToggleBtn').classList.add('d-none');
    document.getElementById('statsToggleBtn').classList.remove('active');
    document.getElementById('playerStatsPanel').classList.add('d-none');
    statsVisible = false;
    stopStatsUpdate();
    resetPlayerStats();
}

// Initialize HLS player for output
function initOutputHlsPlayer(playlistUrl) {
    const video = document.getElementById('outputVideo');
    const qualitySelector = document.getElementById('qualitySelector');
    const ccBtn = document.getElementById('ccBtn');

    if (outputHlsPlayer) {
        outputHlsPlayer.destroy();
        outputHlsPlayer = null;
    }

    // Reset quality selector
    qualitySelector.innerHTML = '<option value="-1">Auto</option>';
    qualitySelector.classList.add('d-none');
    ccBtn.classList.add('d-none');

    if (Hls.isSupported()) {
        outputHlsPlayer = new Hls({
            liveSyncDurationCount: 3,
            liveMaxLatencyDurationCount: 6,
            liveDurationInfinity: true,      // Live stream has infinite duration
            liveBackBufferLength: 0,         // Don't keep back buffer for live
            maxBufferLength: 30,             // Max buffer length
            maxMaxBufferLength: 60,          // Max buffer when switching quality
            enableCEA708Captions: true,      // Enable CEA-608/708 caption extraction
            captionsTextTrack1Label: 'Captions',
            captionsTextTrack1LanguageCode: 'en'
        });

        outputHlsPlayer.loadSource(playlistUrl);
        outputHlsPlayer.attachMedia(video);

        // Hook stats events
        hookHlsStatsEvents(outputHlsPlayer);

        // Handle manifest parsed - populate quality levels
        outputHlsPlayer.on(Hls.Events.MANIFEST_PARSED, function(event, data) {
            // Guard against race condition if stop was clicked
            if (!outputPlayerRunning) return;

            document.getElementById('videoLoadingOverlay').classList.add('d-none');
            video.classList.remove('d-none');
            document.getElementById('playerStatus').className = 'badge bg-success';
            document.getElementById('playerStatus').textContent = 'Playing';
            video.play();

            // Populate quality selector if multiple levels available
            const levels = outputHlsPlayer.levels;
            if (levels && levels.length > 1) {
                qualitySelector.innerHTML = '<option value="-1">Auto</option>';
                levels.forEach((level, index) => {
                    const height = level.height || 'Unknown';
                    const bitrate = level.bitrate ? Math.round(level.bitrate / 1000) + ' kbps' : '';
                    const label = height + 'p' + (bitrate ? ' (' + bitrate + ')' : '');
                    const option = document.createElement('option');
                    option.value = index;
                    option.textContent = label;
                    qualitySelector.appendChild(option);
                });
                qualitySelector.classList.remove('d-none');
                console.log('Quality levels available:', levels.length);
            }
        });

        // Handle subtitle tracks update
        outputHlsPlayer.on(Hls.Events.SUBTITLE_TRACKS_UPDATED, function(event, data) {
            if (data.subtitleTracks && data.subtitleTracks.length > 0) {
                ccBtn.classList.remove('d-none');
                console.log('Subtitle tracks available:', data.subtitleTracks.length);
            }
        });

        // Also check for CEA-608/708 captions via cues
        outputHlsPlayer.on(Hls.Events.CUES_PARSED, function(event, data) {
            if (data.type === 'captions' && data.cues && data.cues.length > 0) {
                ccBtn.classList.remove('d-none');
                console.log('CEA captions detected');
            }
        });

        // Quality selector change handler
        qualitySelector.onchange = function() {
            if (outputHlsPlayer) {
                const level = parseInt(this.value);
                outputHlsPlayer.currentLevel = level;
                console.log('Quality changed to level:', level, level === -1 ? '(Auto)' : '');
            }
        };

        outputHlsPlayer.on(Hls.Events.ERROR, function(event, data) {
            console.error('HLS error:', data);
            if (data.fatal) {
                document.getElementById('playerStatus').className = 'badge bg-danger';
                document.getElementById('playerStatus').textContent = 'Error';
                document.getElementById('videoStatusText').textContent = 'Playback error';
            }
        });
    } else if (video.canPlayType('application/vnd.apple.mpegurl')) {
        // Safari native HLS - quality selection handled by Safari
        video.src = playlistUrl;
        video.addEventListener('loadedmetadata', function() {
            // Guard against race condition if stop was clicked
            if (!outputPlayerRunning) return;

            document.getElementById('videoLoadingOverlay').classList.add('d-none');
            video.classList.remove('d-none');
            document.getElementById('playerStatus').className = 'badge bg-success';
            document.getElementById('playerStatus').textContent = 'Playing';
            video.play();

            // Check for text tracks (captions)
            if (video.textTracks && video.textTracks.length > 0) {
                ccBtn.classList.remove('d-none');
            }
        });
    } else {
        document.getElementById('videoStatusText').textContent = 'HLS not supported in this browser';
        document.getElementById('playerStatus').className = 'badge bg-danger';
        document.getElementById('playerStatus').textContent = 'Unsupported';
    }
}

// Toggle closed captions
function toggleClosedCaptions() {
    const video = document.getElementById('outputVideo');
    const ccBtn = document.getElementById('ccBtn');

    ccEnabled = !ccEnabled;

    if (ccEnabled) {
        ccBtn.classList.remove('btn-outline-secondary');
        ccBtn.classList.add('btn-primary');

        // Enable captions
        if (outputHlsPlayer && outputHlsPlayer.subtitleTracks && outputHlsPlayer.subtitleTracks.length > 0) {
            outputHlsPlayer.subtitleTrack = 0;  // Enable first subtitle track
        }

        // Also try native text tracks
        if (video.textTracks) {
            for (let i = 0; i < video.textTracks.length; i++) {
                if (video.textTracks[i].kind === 'captions' || video.textTracks[i].kind === 'subtitles') {
                    video.textTracks[i].mode = 'showing';
                    break;
                }
            }
        }
    } else {
        ccBtn.classList.remove('btn-primary');
        ccBtn.classList.add('btn-outline-secondary');

        // Disable captions
        if (outputHlsPlayer) {
            outputHlsPlayer.subtitleTrack = -1;  // Disable subtitle track
        }

        // Also disable native text tracks
        if (video.textTracks) {
            for (let i = 0; i < video.textTracks.length; i++) {
                video.textTracks[i].mode = 'hidden';
            }
        }
    }

    console.log('Closed captions:', ccEnabled ? 'enabled' : 'disabled');
}

// ============================================
// HLS Player Statistics
// ============================================

let statsVisible = false;
let statsUpdateInterval = null;
let bandwidthHistory = [];
let fragmentsLoaded = 0;
let stallCount = 0;
let lastDecodedFrames = 0;
let lastFrameTime = 0;
let bandwidthSparklineCtx = null;

// Toggle stats panel visibility
function togglePlayerStats() {
    const panel = document.getElementById('playerStatsPanel');
    const btn = document.getElementById('statsToggleBtn');

    statsVisible = !statsVisible;

    if (statsVisible) {
        panel.classList.remove('d-none');
        btn.classList.add('active');
        startStatsUpdate();
    } else {
        panel.classList.add('d-none');
        btn.classList.remove('active');
        stopStatsUpdate();
    }
}

// Start stats update interval
function startStatsUpdate() {
    if (statsUpdateInterval) return;

    // Initialize sparkline
    const canvas = document.getElementById('bandwidthSparkline');
    if (canvas) {
        bandwidthSparklineCtx = canvas.getContext('2d');
    }

    statsUpdateInterval = setInterval(updatePlayerStats, 500);
    updatePlayerStats(); // Initial update
}

// Stop stats update interval
function stopStatsUpdate() {
    if (statsUpdateInterval) {
        clearInterval(statsUpdateInterval);
        statsUpdateInterval = null;
    }
}

// Update all player stats
function updatePlayerStats() {
    if (!outputHlsPlayer || !outputPlayerRunning) return;

    const video = document.getElementById('outputVideo');

    // Buffer level
    updateBufferStats(video);

    // Latency
    updateLatencyStats();

    // Bandwidth
    updateBandwidthStats();

    // Current quality
    updateQualityStats();

    // Frame stats
    updateFrameStats(video);

    // Network stats
    updateNetworkStats();

    // Quality levels list
    updateQualityLevelsList();
}

// Update buffer gauge
function updateBufferStats(video) {
    if (!video || video.readyState < 2) return;

    const buffered = video.buffered;
    const currentTime = video.currentTime;
    let bufferLength = 0;

    for (let i = 0; i < buffered.length; i++) {
        if (buffered.start(i) <= currentTime && buffered.end(i) > currentTime) {
            bufferLength = buffered.end(i) - currentTime;
            break;
        }
    }

    const bufferValue = document.getElementById('bufferValue');
    const bufferGaugeFill = document.getElementById('bufferGaugeFill');
    const bufferStatus = document.getElementById('bufferStatus');

    bufferValue.textContent = bufferLength.toFixed(1);

    // Max buffer for gauge: 10 seconds
    const bufferPercent = Math.min(100, (bufferLength / 10) * 100);
    bufferGaugeFill.style.width = bufferPercent + '%';

    // Color coding
    bufferGaugeFill.classList.remove('warning', 'critical');
    if (bufferLength < 1) {
        bufferGaugeFill.classList.add('critical');
        bufferStatus.textContent = 'Critical';
    } else if (bufferLength < 3) {
        bufferGaugeFill.classList.add('warning');
        bufferStatus.textContent = 'Low';
    } else {
        bufferStatus.textContent = 'Healthy';
    }
}

// Update latency display
function updateLatencyStats() {
    const latencyValue = document.getElementById('latencyValue');

    if (outputHlsPlayer && outputHlsPlayer.latency !== undefined) {
        latencyValue.textContent = outputHlsPlayer.latency.toFixed(1);
    } else if (outputHlsPlayer && outputHlsPlayer.targetLatency !== undefined) {
        latencyValue.textContent = outputHlsPlayer.targetLatency.toFixed(1);
    } else {
        latencyValue.textContent = '--';
    }
}

// Update bandwidth stats and sparkline
function updateBandwidthStats() {
    const bandwidthValue = document.getElementById('bandwidthValue');

    if (outputHlsPlayer && outputHlsPlayer.bandwidthEstimate) {
        const bwMbps = outputHlsPlayer.bandwidthEstimate / 1000000;
        bandwidthValue.textContent = bwMbps.toFixed(1);

        // Add to history
        bandwidthHistory.push(bwMbps);
        if (bandwidthHistory.length > 30) {
            bandwidthHistory.shift();
        }

        // Draw sparkline
        drawBandwidthSparkline();
    } else {
        bandwidthValue.textContent = '--';
    }
}

// Draw bandwidth sparkline
function drawBandwidthSparkline() {
    if (!bandwidthSparklineCtx || bandwidthHistory.length < 2) return;

    const canvas = bandwidthSparklineCtx.canvas;
    const width = canvas.width = canvas.offsetWidth * 2;
    const height = canvas.height = 48;

    bandwidthSparklineCtx.clearRect(0, 0, width, height);

    const max = Math.max(...bandwidthHistory) * 1.1 || 1;
    const min = 0;
    const range = max - min;

    const stepX = width / (bandwidthHistory.length - 1);

    // Draw line
    bandwidthSparklineCtx.beginPath();
    bandwidthSparklineCtx.strokeStyle = '#10b981';
    bandwidthSparklineCtx.lineWidth = 2;

    bandwidthHistory.forEach((val, i) => {
        const x = i * stepX;
        const y = height - ((val - min) / range) * (height - 4) - 2;

        if (i === 0) {
            bandwidthSparklineCtx.moveTo(x, y);
        } else {
            bandwidthSparklineCtx.lineTo(x, y);
        }
    });

    bandwidthSparklineCtx.stroke();

    // Draw fill
    bandwidthSparklineCtx.lineTo(width, height);
    bandwidthSparklineCtx.lineTo(0, height);
    bandwidthSparklineCtx.closePath();

    const gradient = bandwidthSparklineCtx.createLinearGradient(0, 0, 0, height);
    gradient.addColorStop(0, 'rgba(16, 185, 129, 0.3)');
    gradient.addColorStop(1, 'rgba(16, 185, 129, 0.05)');
    bandwidthSparklineCtx.fillStyle = gradient;
    bandwidthSparklineCtx.fill();
}

// Update current quality display
function updateQualityStats() {
    const qualityValue = document.getElementById('currentQualityValue');
    const qualityBitrate = document.getElementById('currentQualityBitrate');
    const abrIndicator = document.getElementById('abrModeIndicator');

    if (outputHlsPlayer && outputHlsPlayer.levels && outputHlsPlayer.currentLevel >= 0) {
        const level = outputHlsPlayer.levels[outputHlsPlayer.currentLevel];
        if (level) {
            qualityValue.textContent = (level.height || 'Auto') + 'p';
            const bitrateMbps = (level.bitrate / 1000000).toFixed(1);
            qualityBitrate.textContent = bitrateMbps + ' Mbps';
        }
    } else {
        qualityValue.textContent = 'Auto';
        qualityBitrate.textContent = '--';
    }

    // ABR mode indicator
    if (outputHlsPlayer) {
        const isAuto = outputHlsPlayer.autoLevelEnabled;
        abrIndicator.textContent = isAuto ? 'Auto ABR' : 'Manual';
        abrIndicator.className = isAuto ? 'badge bg-success ms-auto' : 'badge bg-secondary ms-auto';
    }
}

// Update frame statistics
function updateFrameStats(video) {
    const framesDecoded = document.getElementById('framesDecoded');
    const framesDropped = document.getElementById('framesDropped');
    const framesDroppedIndicator = document.getElementById('framesDroppedIndicator');
    const currentFps = document.getElementById('currentFps');

    if (video.getVideoPlaybackQuality) {
        const quality = video.getVideoPlaybackQuality();

        framesDecoded.textContent = quality.totalVideoFrames.toLocaleString();
        framesDropped.textContent = quality.droppedVideoFrames.toLocaleString();

        // Dropped frames indicator
        const dropRate = quality.totalVideoFrames > 0
            ? (quality.droppedVideoFrames / quality.totalVideoFrames) * 100
            : 0;

        framesDroppedIndicator.className = 'status-dot';
        if (dropRate < 0.1) {
            framesDroppedIndicator.classList.add('status-dot-ok');
        } else if (dropRate < 1) {
            framesDroppedIndicator.classList.add('status-dot-warning');
        } else {
            framesDroppedIndicator.classList.add('status-dot-error');
        }

        // Calculate FPS
        const now = performance.now();
        if (lastFrameTime > 0) {
            const framesDelta = quality.totalVideoFrames - lastDecodedFrames;
            const timeDelta = (now - lastFrameTime) / 1000;
            if (timeDelta > 0) {
                const fps = framesDelta / timeDelta;
                currentFps.textContent = fps.toFixed(1);
            }
        }
        lastDecodedFrames = quality.totalVideoFrames;
        lastFrameTime = now;
    }
}

// Update network stats
function updateNetworkStats() {
    const ttfbValue = document.getElementById('ttfbValue');
    const fragmentsLoadedEl = document.getElementById('fragmentsLoaded');
    const stallCountEl = document.getElementById('stallCount');
    const stallIndicator = document.getElementById('stallIndicator');

    if (outputHlsPlayer && outputHlsPlayer.ttfbEstimate) {
        ttfbValue.textContent = Math.round(outputHlsPlayer.ttfbEstimate);
    }

    fragmentsLoadedEl.textContent = fragmentsLoaded.toLocaleString();
    stallCountEl.textContent = stallCount.toLocaleString();

    // Stall indicator
    stallIndicator.className = 'status-dot';
    if (stallCount === 0) {
        stallIndicator.classList.add('status-dot-ok');
    } else if (stallCount < 3) {
        stallIndicator.classList.add('status-dot-warning');
    } else {
        stallIndicator.classList.add('status-dot-error');
    }
}

// Update quality levels visual list
function updateQualityLevelsList() {
    const list = document.getElementById('qualityLevelsList');
    if (!outputHlsPlayer || !outputHlsPlayer.levels || outputHlsPlayer.levels.length === 0) {
        list.innerHTML = '<div class="text-muted small">No quality levels available</div>';
        return;
    }

    const levels = outputHlsPlayer.levels;
    const currentLevel = outputHlsPlayer.currentLevel;
    const maxBitrate = Math.max(...levels.map(l => l.bitrate || 0));

    let html = '';
    levels.forEach((level, index) => {
        const isActive = index === currentLevel;
        const height = level.height || 'Unknown';
        const bitrateMbps = ((level.bitrate || 0) / 1000000).toFixed(1);
        const barWidth = maxBitrate > 0 ? ((level.bitrate || 0) / maxBitrate) * 100 : 0;

        html += `
            <div class="quality-level-item ${isActive ? 'active' : ''}">
                <div class="quality-level-indicator"></div>
                <div class="quality-level-info">
                    <span class="quality-level-resolution">${height}p</span>
                    <span class="quality-level-bitrate">${bitrateMbps} Mbps</span>
                </div>
                <div class="quality-level-bar-container">
                    <div class="quality-level-bar" style="width: ${barWidth}%"></div>
                </div>
            </div>
        `;
    });

    list.innerHTML = html;
}

// Reset player stats
function resetPlayerStats() {
    bandwidthHistory = [];
    fragmentsLoaded = 0;
    stallCount = 0;
    lastDecodedFrames = 0;
    lastFrameTime = 0;

    // Reset UI
    document.getElementById('bufferValue').textContent = '0.0';
    document.getElementById('bufferGaugeFill').style.width = '0%';
    document.getElementById('bufferStatus').textContent = 'Waiting';
    document.getElementById('latencyValue').textContent = '--';
    document.getElementById('bandwidthValue').textContent = '--';
    document.getElementById('currentQualityValue').textContent = '--';
    document.getElementById('currentQualityBitrate').textContent = '--';
    document.getElementById('framesDecoded').textContent = '0';
    document.getElementById('framesDropped').textContent = '0';
    document.getElementById('currentFps').textContent = '--';
    document.getElementById('ttfbValue').textContent = '--';
    document.getElementById('fragmentsLoaded').textContent = '0';
    document.getElementById('stallCount').textContent = '0';
    document.getElementById('qualityLevelsList').innerHTML = '';
}

// Hook HLS events for stats (called from initOutputHlsPlayer)
function hookHlsStatsEvents(hls) {
    // Show stats button when player is ready
    document.getElementById('statsToggleBtn').classList.remove('d-none');

    // Fragment loaded event
    hls.on(Hls.Events.FRAG_LOADED, function(event, data) {
        fragmentsLoaded++;
    });

    // Buffer stalled event
    hls.on(Hls.Events.ERROR, function(event, data) {
        if (data.details === 'bufferStalledError') {
            stallCount++;
        }
    });

    // Level switched event
    hls.on(Hls.Events.LEVEL_SWITCHED, function(event, data) {
        console.log('Level switched to:', data.level);
        if (statsVisible) {
            updateQualityStats();
            updateQualityLevelsList();
        }
    });
}
</script>

<!-- HLS.js Library -->
<script src="https://cdnjs.cloudflare.com/ajax/libs/hls.js/1.6.0-beta.1.0.canary.10759/hls.min.js"></script>

<?php include __DIR__ . '/../templates/footer.php'; ?>
