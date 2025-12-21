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
    <div class="d-flex justify-content-between align-items-center mb-3">
        <h2 class="mb-0"><i class="bi bi-download me-2"></i>Input Sources</h2>
        <button class="btn btn-primary" data-bs-toggle="modal" data-bs-target="#addInputModal">
            <i class="bi bi-plus-lg me-1"></i>Add Input
        </button>
    </div>

    <!-- Search and Filter Bar -->
    <div class="card mb-3">
        <div class="card-body py-2">
            <div class="row align-items-center">
                <div class="col-md-4">
                    <div class="input-group">
                        <span class="input-group-text bg-transparent border-end-0"><i class="bi bi-search"></i></span>
                        <input type="text" class="form-control border-start-0" id="searchInput" placeholder="Search inputs..." onkeyup="filterInputs()">
                    </div>
                </div>
                <div class="col-md-3">
                    <select class="form-select" id="typeFilter" onchange="filterInputs()">
                        <option value="">All Types</option>
                        <option value="udp">UDP</option>
                        <option value="srt">SRT</option>
                        <option value="rist">RIST</option>
                        <option value="rtmp">RTMP</option>
                        <option value="hls">HLS</option>
                        <option value="file">File</option>
                    </select>
                </div>
                <div class="col-md-3">
                    <select class="form-select" id="statusFilter" onchange="filterInputs()">
                        <option value="">All Status</option>
                        <option value="running">Running</option>
                        <option value="stopped">Stopped</option>
                        <option value="error">Error</option>
                    </select>
                </div>
                <div class="col-md-2 text-end">
                    <span class="text-muted" id="inputCount"><?php echo count($inputs); ?> inputs</span>
                </div>
            </div>
        </div>
    </div>

    <!-- Inputs Table -->
    <div class="card">
        <div class="table-responsive">
            <table class="table align-middle mb-0" id="inputsTable">
                <thead class="table-light">
                    <tr>
                        <th style="width: 40px;"></th>
                        <th>Name</th>
                        <th style="width: 80px;">Type</th>
                        <th>Source</th>
                        <th>Output</th>
                        <th style="width: 180px;">Bitrate (V/A)</th>
                        <th style="width: 150px;">Actions</th>
                    </tr>
                </thead>
                <tbody>
                    <?php if (empty($inputs)): ?>
                    <tr id="emptyRow">
                        <td colspan="7" class="text-center py-5">
                            <i class="bi bi-download text-muted" style="font-size: 2rem;"></i>
                            <p class="mt-2 mb-0 text-muted">No input sources configured</p>
                            <button class="btn btn-primary btn-sm mt-2" data-bs-toggle="modal" data-bs-target="#addInputModal">
                                <i class="bi bi-plus-lg me-1"></i>Add Input
                            </button>
                        </td>
                    </tr>
                    <?php else: ?>
                    <?php $rowNum = 0; foreach ($inputs as $input): $rowNum++;
                        $outputAddr = ($input['config']['output']['address'] ?? '') . ':' . ($input['config']['output']['port'] ?? '');
                        $apiPort = $input['config']['output']['api_port'] ?? null;
                    ?>
                    <tr class="input-row <?php echo ($rowNum % 2 == 0) ? 'row-even' : 'row-odd'; ?>"
                        data-id="<?php echo htmlspecialchars($input['id']); ?>"
                        data-name="<?php echo htmlspecialchars(strtolower($input['name'])); ?>"
                        data-type="<?php echo htmlspecialchars(strtolower($input['type'] ?? 'udp')); ?>"
                        data-status="<?php echo htmlspecialchars($input['status']); ?>"
                        data-source="<?php echo htmlspecialchars(strtolower($input['source'] ?? '')); ?>"
                        data-api-port="<?php echo htmlspecialchars($apiPort ?? ''); ?>">
                        <td>
                            <span class="status-dot status-<?php echo $input['status']; ?>" id="status-<?php echo $input['id']; ?>" title="<?php echo ucfirst($input['status']); ?>"></span>
                        </td>
                        <td>
                            <strong><?php echo htmlspecialchars($input['name']); ?></strong>
                        </td>
                        <td>
                            <span class="badge bg-<?php echo getTypeBadgeColor($input['type'] ?? 'udp'); ?>"><?php echo htmlspecialchars(strtoupper($input['type'] ?? 'UDP')); ?></span>
                        </td>
                        <td class="text-truncate" style="max-width: 200px;" title="<?php echo htmlspecialchars($input['source'] ?? ''); ?>">
                            <small class="text-muted"><?php echo htmlspecialchars($input['source'] ?? 'Not configured'); ?></small>
                        </td>
                        <td>
                            <?php if ($outputAddr && $outputAddr !== ':'): ?>
                            <small class="text-muted font-monospace"><?php echo htmlspecialchars($outputAddr); ?></small>
                            <?php else: ?>
                            <small class="text-muted">-</small>
                            <?php endif; ?>
                        </td>
                        <td>
                            <div class="bitrate-cell" id="bitrate-<?php echo $input['id']; ?>">
                                <?php if ($apiPort): ?>
                                <span class="bitrate-video">-</span> / <span class="bitrate-audio">-</span>
                                <button class="btn btn-link btn-sm p-0 ms-2 preview-btn" onclick="showPreview('<?php echo $input['id']; ?>', '<?php echo htmlspecialchars($input['name']); ?>')" title="Preview stream">
                                    <i class="bi bi-play-circle"></i>
                                </button>
                                <?php else: ?>
                                <small class="text-muted">N/A</small>
                                <?php endif; ?>
                            </div>
                        </td>
                        <td>
                            <div class="btn-group btn-group-sm">
                                <?php if ($input['status'] === 'running'): ?>
                                <button class="btn btn-outline-warning" onclick="stopService('inputs', '<?php echo $input['id']; ?>')" title="Stop">
                                    <i class="bi bi-stop-fill"></i>
                                </button>
                                <?php else: ?>
                                <button class="btn btn-outline-success" onclick="startService('inputs', '<?php echo $input['id']; ?>')" title="Start">
                                    <i class="bi bi-play-fill"></i>
                                </button>
                                <?php endif; ?>
                                <button class="btn btn-outline-secondary" onclick="editInput('<?php echo $input['id']; ?>')" title="Edit">
                                    <i class="bi bi-gear"></i>
                                </button>
                                <button class="btn btn-outline-danger" onclick="deleteService('inputs', '<?php echo $input['id']; ?>')" title="Delete">
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

<?php
function getTypeBadgeColor($type) {
    $colors = [
        'udp' => 'primary',
        'srt' => 'success',
        'rist' => 'info',
        'rtmp' => 'warning',
        'hls' => 'secondary',
        'file' => 'dark'
    ];
    return $colors[strtolower($type)] ?? 'primary';
}
?>

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
                        <select class="form-select" id="configProgramSelect" onchange="filterPidsByProgram(this.value)">
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

<!-- Preview Modal -->
<div class="modal fade" id="previewModal" tabindex="-1">
    <div class="modal-dialog modal-xl">
        <div class="modal-content">
            <div class="modal-header">
                <h5 class="modal-title"><i class="bi bi-play-circle me-2"></i>Preview - <span id="previewInputName"></span></h5>
                <button type="button" class="btn-close" data-bs-dismiss="modal"></button>
            </div>
            <div class="modal-body">
                <input type="hidden" id="previewInputId">

                <!-- Video Player and Stream Info Row -->
                <div class="row mb-3">
                    <!-- Video Player Section -->
                    <div class="col-lg-8">
                        <div id="videoContainer" class="position-relative bg-dark rounded" style="aspect-ratio: 16/9; max-height: 400px;">
                            <!-- Placeholder with Play Button -->
                            <div id="videoPlaceholder" class="position-absolute top-0 start-0 w-100 h-100 d-flex flex-column align-items-center justify-content-center text-white">
                                <i class="bi bi-play-circle display-1 mb-3"></i>
                                <span id="videoStatusText">Click to start preview</span>
                            </div>
                            <!-- Loading Spinner -->
                            <div id="videoLoading" class="position-absolute top-0 start-0 w-100 h-100 d-none flex-column align-items-center justify-content-center text-white">
                                <div class="spinner-border text-light mb-3" role="status"></div>
                                <span>Loading stream...</span>
                                <small class="text-muted mt-2" id="videoLoadingStatus">Waiting for segments...</small>
                            </div>
                            <!-- Video Element -->
                            <video id="previewVideo" class="w-100 h-100 d-none" controls autoplay muted playsinline></video>
                        </div>
                    </div>

                    <!-- Stream Info Panel -->
                    <div class="col-lg-4">
                        <div class="card h-100">
                            <div class="card-header py-2">
                                <strong><i class="bi bi-info-circle me-1"></i>Stream Info</strong>
                                <button class="btn btn-sm btn-outline-secondary float-end" onclick="refreshMediaInfo()" title="Refresh">
                                    <i class="bi bi-arrow-clockwise"></i>
                                </button>
                            </div>
                            <div class="card-body p-2" id="streamInfoBody">
                                <div class="text-center text-muted py-4" id="streamInfoLoading">
                                    <div class="spinner-border spinner-border-sm me-2" role="status"></div>
                                    Loading stream info...
                                </div>
                                <div id="streamInfoContent" style="display: none;">
                                    <!-- Video Info -->
                                    <div class="mb-3">
                                        <h6 class="text-primary mb-2"><i class="bi bi-camera-video me-1"></i>Video</h6>
                                        <table class="table table-sm table-borderless mb-0">
                                            <tr><td class="text-muted" style="width:40%">Codec</td><td id="infoVideoCodec">-</td></tr>
                                            <tr><td class="text-muted">Resolution</td><td id="infoVideoRes">-</td></tr>
                                            <tr><td class="text-muted">Frame Rate</td><td id="infoVideoFps">-</td></tr>
                                            <tr><td class="text-muted">Profile</td><td id="infoVideoProfile">-</td></tr>
                                        </table>
                                    </div>
                                    <!-- Audio Info -->
                                    <div>
                                        <h6 class="text-success mb-2"><i class="bi bi-volume-up me-1"></i>Audio</h6>
                                        <div id="infoAudioTracks">
                                            <!-- Audio tracks will be inserted here -->
                                        </div>
                                    </div>
                                </div>
                                <div id="streamInfoError" class="text-danger text-center py-3" style="display: none;">
                                    <i class="bi bi-exclamation-triangle me-1"></i>
                                    <span id="streamInfoErrorText">Failed to load</span>
                                </div>
                            </div>
                        </div>
                    </div>
                </div>

                <!-- Bitrate Stats -->
                <div class="row mb-3">
                    <div class="col-md-6">
                        <div class="card bg-light">
                            <div class="card-body py-2">
                                <div class="d-flex justify-content-between align-items-center">
                                    <span class="text-muted">Video Bitrate</span>
                                    <span class="fw-bold text-primary" id="graphVideoBitrate">-</span>
                                </div>
                                <div class="d-flex justify-content-between align-items-center mt-1">
                                    <small class="text-muted">PID</small>
                                    <small id="graphVideoPid">-</small>
                                </div>
                            </div>
                        </div>
                    </div>
                    <div class="col-md-6">
                        <div class="card bg-light">
                            <div class="card-body py-2">
                                <div class="d-flex justify-content-between align-items-center">
                                    <span class="text-muted">Audio Bitrate</span>
                                    <span class="fw-bold text-success" id="graphAudioBitrate">-</span>
                                </div>
                                <div class="d-flex justify-content-between align-items-center mt-1">
                                    <small class="text-muted">PID(s)</small>
                                    <small id="graphAudioPid">-</small>
                                </div>
                            </div>
                        </div>
                    </div>
                </div>

                <!-- Bitrate Graph Canvas -->
                <div class="card mb-3">
                    <div class="card-header py-2 d-flex justify-content-between align-items-center">
                        <strong><i class="bi bi-graph-up me-1"></i>Bitrate Monitor</strong>
                        <small class="text-muted">
                            <span id="graphStatus" class="badge bg-success">Live</span>
                            Last update: <span id="graphLastUpdate">-</span>
                        </small>
                    </div>
                    <div class="card-body">
                        <div class="position-relative" style="height: 180px;">
                            <canvas id="bitrateChart"></canvas>
                        </div>
                    </div>
                </div>

                <!-- A/V Sync Section -->
                <div class="card mb-3">
                    <div class="card-header py-2 d-flex justify-content-between align-items-center">
                        <strong><i class="bi bi-soundwave me-1"></i>A/V Sync Monitor</strong>
                        <small class="text-muted">
                            <span id="avsyncStatus" class="badge bg-secondary">Loading...</span>
                            Updated: <span id="avsyncLastUpdate">-</span>
                        </small>
                    </div>
                    <div class="card-body">
                        <!-- A/V Sync Stats -->
                        <div class="row mb-3">
                            <div class="col-md-4">
                                <div class="avsync-stat-card">
                                    <div class="stat-label">A→V Mean</div>
                                    <div class="stat-value" id="avsyncA2V">-</div>
                                    <div class="stat-sublabel">Audio to Video gap</div>
                                </div>
                            </div>
                            <div class="col-md-4">
                                <div class="avsync-stat-card">
                                    <div class="stat-label">V→A Mean</div>
                                    <div class="stat-value" id="avsyncV2A">-</div>
                                    <div class="stat-sublabel">Video to Audio gap</div>
                                </div>
                            </div>
                            <div class="col-md-4">
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
                        <div class="text-center mt-2">
                            <small class="text-muted">24-hour A/V sync history (polled every 5 minutes)</small>
                        </div>
                    </div>
                </div>

                <!-- Output Info -->
                <div class="d-flex justify-content-end">
                    <small class="text-muted">
                        Output: <span id="graphOutputAddr" class="font-monospace">-</span>
                    </small>
                </div>
            </div>
            <div class="modal-footer">
                <button type="button" class="btn btn-secondary" data-bs-dismiss="modal">Close</button>
            </div>
        </div>
    </div>
</div>

<!-- HLS.js Library -->
<script src="https://cdnjs.cloudflare.com/ajax/libs/hls.js/1.6.0-beta.1.0.canary.10759/hls.min.js"></script>

<!-- Chart.js for graphs -->
<script src="https://cdn.jsdelivr.net/npm/chart.js@4.4.1/dist/chart.umd.min.js"></script>

<style>
/* Status dots */
.status-dot {
    display: inline-block;
    width: 10px;
    height: 10px;
    border-radius: 50%;
    background-color: #9ca3af;
}
.status-dot.status-running {
    background-color: #10b981;
    box-shadow: 0 0 0 3px rgba(16, 185, 129, 0.25);
    animation: pulse 2s infinite;
}
.status-dot.status-stopped {
    background-color: #9ca3af;
}
.status-dot.status-error {
    background-color: #ef4444;
    box-shadow: 0 0 0 3px rgba(239, 68, 68, 0.25);
}

@keyframes pulse {
    0%, 100% { opacity: 1; }
    50% { opacity: 0.5; }
}

/* Modern card styling */
.card {
    border: none;
    box-shadow: 0 4px 6px -1px rgba(0, 0, 0, 0.1), 0 2px 4px -1px rgba(0, 0, 0, 0.06);
    border-radius: 0.75rem;
    overflow: hidden;
}

/* Table container */
.table-responsive {
    border-radius: 0 0 0.75rem 0.75rem;
}

#inputsTable {
    border-collapse: separate;
    border-spacing: 0;
    margin-bottom: 0;
}

/* Table header - gradient blue */
#inputsTable thead th {
    font-weight: 600;
    font-size: 0.7rem;
    text-transform: uppercase;
    letter-spacing: 0.08em;
    color: #ffffff;
    background: linear-gradient(135deg, #1e3a5f 0%, #2563eb 100%);
    border-bottom: none;
    padding: 1rem 1rem;
    white-space: nowrap;
}

#inputsTable thead th:first-child {
    border-radius: 0;
}
#inputsTable thead th:last-child {
    border-radius: 0;
}

/* Alternating row colors */
.table > tbody > tr.row-odd > td {
    background-color: #ffffff !important;
}
.table > tbody > tr.row-even > td {
    background-color: #f0f7ff !important;
}
.table > tbody > tr.input-row:hover > td {
    background-color: #dbeafe !important;
}

#inputsTable tbody td {
    padding: 0.875rem 1rem;
    border-bottom: 1px solid #e5e7eb;
    vertical-align: middle;
    transition: background-color 0.15s ease;
}

#inputsTable tbody tr:last-child td {
    border-bottom: none;
}

/* Name column - bold and prominent */
#inputsTable tbody td:nth-child(2) strong {
    color: #1e293b;
    font-weight: 600;
    font-size: 0.95rem;
}

/* Source column - monospace with better color */
#inputsTable tbody td small.text-muted {
    color: #475569 !important;
    font-family: 'SF Mono', 'Monaco', 'Menlo', 'Ubuntu Mono', monospace;
    font-size: 0.8rem;
    background: rgba(0,0,0,0.04);
    padding: 0.2rem 0.4rem;
    border-radius: 0.25rem;
}

/* Type badges - colorful and modern */
.badge {
    font-weight: 600;
    font-size: 0.65rem;
    padding: 0.4em 0.7em;
    letter-spacing: 0.05em;
    border-radius: 0.375rem;
    text-shadow: 0 1px 1px rgba(0,0,0,0.1);
}
.badge.bg-primary { background: linear-gradient(135deg, #3b82f6 0%, #1d4ed8 100%) !important; }
.badge.bg-success { background: linear-gradient(135deg, #22c55e 0%, #16a34a 100%) !important; }
.badge.bg-info { background: linear-gradient(135deg, #06b6d4 0%, #0891b2 100%) !important; }
.badge.bg-warning { background: linear-gradient(135deg, #f59e0b 0%, #d97706 100%) !important; color: #fff !important; }
.badge.bg-secondary { background: linear-gradient(135deg, #6b7280 0%, #4b5563 100%) !important; }
.badge.bg-dark { background: linear-gradient(135deg, #374151 0%, #1f2937 100%) !important; }

/* Action buttons - cleaner look */
.btn-group-sm .btn {
    padding: 0.4rem 0.6rem;
    border-radius: 0.375rem;
    font-size: 0.8rem;
    border-width: 1.5px;
}
.btn-group-sm .btn:not(:last-child) {
    border-top-right-radius: 0;
    border-bottom-right-radius: 0;
    margin-right: -1px;
}
.btn-group-sm .btn:not(:first-child) {
    border-top-left-radius: 0;
    border-bottom-left-radius: 0;
}
.btn-group-sm .btn i {
    font-size: 0.85rem;
}

/* Bitrate/Packets columns - monospace numbers */
#inputsTable tbody td:nth-child(5),
#inputsTable tbody td:nth-child(6) {
    font-family: 'SF Mono', 'Monaco', 'Menlo', monospace;
    font-size: 0.85rem;
    color: #374151;
}

/* Search/filter bar */
.card.mb-3 {
    background: linear-gradient(135deg, #f8fafc 0%, #eef2f7 100%);
    border: 1px solid #e2e8f0;
}

/* Wizard styles */
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

/* Bitrate cell styling */
.bitrate-cell {
    font-family: 'SF Mono', 'Monaco', 'Menlo', monospace;
    font-size: 0.85rem;
    white-space: nowrap;
}
.bitrate-cell .bitrate-video {
    color: #2563eb;
    font-weight: 600;
}
.bitrate-cell .bitrate-audio {
    color: #16a34a;
    font-weight: 600;
}
.bitrate-cell .graph-btn {
    color: #6b7280;
    transition: color 0.15s;
}
.bitrate-cell .graph-btn:hover {
    color: #2563eb;
}
.bitrate-cell.offline {
    opacity: 0.5;
}
.source-type-settings {
    background: #fff;
    border: 1px solid #e9ecef;
    border-radius: 0.25rem;
    padding: 0.75rem;
    margin-top: 0.5rem;
}

/* ============ Enhanced Modal Styles ============ */

/* Modal header with gradient */
#previewModal .modal-header {
    background: linear-gradient(135deg, #1e3a5f 0%, #2563eb 100%);
    color: white;
    border-bottom: none;
    padding: 1rem 1.5rem;
}
#previewModal .modal-header .modal-title {
    font-weight: 600;
}
#previewModal .modal-header .btn-close {
    filter: invert(1);
    opacity: 0.8;
}
#previewModal .modal-header .btn-close:hover {
    opacity: 1;
}

/* Modal body */
#previewModal .modal-body {
    background: linear-gradient(180deg, #f8fafc 0%, #ffffff 100%);
    padding: 1.5rem;
}

/* Modal footer */
#previewModal .modal-footer {
    background: #f8fafc;
    border-top: 1px solid #e2e8f0;
}

/* Video container styling */
#videoContainer {
    border-radius: 0.75rem;
    overflow: hidden;
    box-shadow: 0 4px 6px -1px rgba(0, 0, 0, 0.1), 0 2px 4px -1px rgba(0, 0, 0, 0.06);
}

/* Stream info panel enhanced */
#previewModal .card {
    border: none;
    box-shadow: 0 1px 3px 0 rgba(0, 0, 0, 0.1), 0 1px 2px 0 rgba(0, 0, 0, 0.06);
    border-radius: 0.75rem;
}
#previewModal .card-header {
    background: linear-gradient(135deg, #f8fafc 0%, #f1f5f9 100%);
    border-bottom: 1px solid #e2e8f0;
    font-size: 0.875rem;
}

/* Bitrate stat cards */
#previewModal .card.bg-light {
    background: linear-gradient(135deg, #f8fafc 0%, #f1f5f9 100%) !important;
    border: 1px solid #e2e8f0;
}

/* ============ A/V Sync Styles ============ */

.avsync-stat-card {
    background: linear-gradient(135deg, #f8fafc 0%, #f1f5f9 100%);
    border: 1px solid #e2e8f0;
    border-radius: 0.75rem;
    padding: 1rem;
    text-align: center;
    transition: all 0.2s ease;
}
.avsync-stat-card:hover {
    box-shadow: 0 4px 6px -1px rgba(0, 0, 0, 0.1);
    transform: translateY(-1px);
}
.avsync-stat-card .stat-label {
    font-size: 0.75rem;
    font-weight: 600;
    text-transform: uppercase;
    letter-spacing: 0.05em;
    color: #64748b;
    margin-bottom: 0.25rem;
}
.avsync-stat-card .stat-value {
    font-size: 1.5rem;
    font-weight: 700;
    font-family: 'SF Mono', 'Monaco', 'Menlo', monospace;
    color: #1e293b;
}
.avsync-stat-card .stat-sublabel {
    font-size: 0.7rem;
    color: #94a3b8;
    margin-top: 0.25rem;
}

/* A/V Sync status colors */
.avsync-stat-card .stat-value.status-ok { color: #16a34a; }
.avsync-stat-card .stat-value.status-warning { color: #d97706; }
.avsync-stat-card .stat-value.status-error { color: #dc2626; }

/* Status badges */
.badge.avsync-ok {
    background: linear-gradient(135deg, #22c55e 0%, #16a34a 100%) !important;
}
.badge.avsync-warning {
    background: linear-gradient(135deg, #f59e0b 0%, #d97706 100%) !important;
}
.badge.avsync-error {
    background: linear-gradient(135deg, #ef4444 0%, #dc2626 100%) !important;
}
</style>

<script>
let currentStep = 1;
let nameCheckTimeout = null;
let sourcesCount = 0;

// Filter inputs based on search and dropdowns
function filterInputs() {
    const searchText = document.getElementById('searchInput').value.toLowerCase();
    const typeFilter = document.getElementById('typeFilter').value.toLowerCase();
    const statusFilter = document.getElementById('statusFilter').value.toLowerCase();

    const rows = document.querySelectorAll('.input-row');
    let visibleCount = 0;

    rows.forEach(row => {
        const name = row.dataset.name || '';
        const type = row.dataset.type || '';
        const status = row.dataset.status || '';
        const source = row.dataset.source || '';

        const matchesSearch = !searchText ||
            name.includes(searchText) ||
            source.includes(searchText);
        const matchesType = !typeFilter || type === typeFilter;
        const matchesStatus = !statusFilter || status === statusFilter;

        if (matchesSearch && matchesType && matchesStatus) {
            row.style.display = '';
            visibleCount++;
        } else {
            row.style.display = 'none';
        }
    });

    // Update count
    document.getElementById('inputCount').textContent = visibleCount + ' input' + (visibleCount !== 1 ? 's' : '');
}

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

// Store current scan data for program-based filtering
let currentScanData = null;

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
    console.log('populateConfigPidSelects: Received data', data);

    // Store scan data for program filtering
    currentScanData = data;

    const programSelect = document.getElementById('configProgramSelect');
    const videoSelect = document.getElementById('configVideoSelect');
    const audioSelect = document.getElementById('configAudioSelect');
    const resultsContent = document.getElementById('configScanResultsContent');

    // Clear ALL options - video/audio stay empty until program is selected
    programSelect.innerHTML = '<option value="">-- Select program first --</option>';
    videoSelect.innerHTML = '<option value="">-- Select program first --</option>';
    audioSelect.innerHTML = '';

    // Populate programs
    if (data.programs && data.programs.length > 0) {
        console.log('populateConfigPidSelects: Found', data.programs.length, 'programs');
        data.programs.forEach(prog => {
            const opt = document.createElement('option');
            opt.value = prog.pmt_pid;  // Use PMT PID, not program number
            opt.textContent = prog.name || `Program ${prog.id}`;
            if (prog.provider) {
                opt.textContent += ` (${prog.provider})`;
            }
            opt.textContent += ` (PMT: ${prog.pmt_pid})`;
            programSelect.appendChild(opt);
        });

        // Auto-select and populate PIDs only if exactly one program
        if (data.programs.length === 1) {
            console.log('populateConfigPidSelects: Auto-selecting single program');
            programSelect.value = data.programs[0].pmt_pid;
            filterPidsByProgram(data.programs[0].pmt_pid);
        }
    } else {
        console.log('populateConfigPidSelects: No programs found');
    }

    // Show results summary
    let html = '<div class="row">';
    html += `<div class="col-md-4"><strong>Programs:</strong> ${data.programs?.length || 0}</div>`;
    html += `<div class="col-md-4"><strong>Video:</strong> ${data.all_video_pids?.length || 0}</div>`;
    html += `<div class="col-md-4"><strong>Audio:</strong> ${data.all_audio_pids?.length || 0}</div>`;
    html += '</div>';

    // Show program details
    if (data.programs && data.programs.length > 0) {
        html += '<div class="mt-2">';
        data.programs.forEach(prog => {
            html += `<div class="mb-1"><strong>${prog.name}</strong>: `;
            html += `${prog.video_pids?.length || 0} video, ${prog.audio_pids?.length || 0} audio`;
            html += '</div>';
        });
        html += '</div>';
    }

    resultsContent.innerHTML = html;
}

// Filter video and audio PIDs based on selected program (by PMT PID)
function filterPidsByProgram(pmtPid) {
    const videoSelect = document.getElementById('configVideoSelect');
    const audioSelect = document.getElementById('configAudioSelect');

    // Always clear existing options first
    videoSelect.innerHTML = '<option value="">-- Select video PID --</option>';
    audioSelect.innerHTML = '';

    // If no program selected or no scan data, just leave empty
    if (!pmtPid || !currentScanData || !currentScanData.programs) {
        console.log('filterPidsByProgram: No program selected or no scan data');
        return;
    }

    // Convert to number for comparison (API returns numbers, select value is string)
    const pmtPidNum = parseInt(pmtPid, 10);
    console.log('filterPidsByProgram: Looking for program with PMT PID', pmtPidNum, 'in', currentScanData.programs.map(p => p.pmt_pid));

    // Find the selected program by PMT PID
    const program = currentScanData.programs.find(p => p.pmt_pid === pmtPidNum);
    if (!program) {
        console.log('filterPidsByProgram: Program not found');
        return;
    }

    console.log('filterPidsByProgram: Found program', program.name, 'with', program.video_pids?.length, 'video and', program.audio_pids?.length, 'audio PIDs');

    // Populate video PIDs for this program
    if (program.video_pids && program.video_pids.length > 0) {
        program.video_pids.forEach(vid => {
            const opt = document.createElement('option');
            opt.value = vid.pid;
            opt.textContent = `PID ${vid.pid} - ${vid.description || vid.codec || 'Video'}`;
            videoSelect.appendChild(opt);
        });
        // Auto-select first video
        if (program.video_pids.length === 1) {
            videoSelect.value = program.video_pids[0].pid;
        }
    }

    // Populate audio PIDs for this program
    if (program.audio_pids && program.audio_pids.length > 0) {
        program.audio_pids.forEach(aud => {
            const opt = document.createElement('option');
            opt.value = aud.pid;
            opt.textContent = `PID ${aud.pid} - ${aud.description || aud.codec || 'Audio'} (${aud.language || 'und'})`;
            audioSelect.appendChild(opt);
        });
        // Auto-select all audio tracks
        Array.from(audioSelect.options).forEach(opt => opt.selected = true);
    }
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

// ============ Metrics, Preview and Bitrate Graph ============

let metricsInterval = null;
let bitrateChart = null;
let avsyncChart = null;
let previewModal = null;
let graphUpdateInterval = null;
let avsyncUpdateInterval = null;
let previewKeepaliveInterval = null;
let previewStatusInterval = null;
let hlsPlayer = null;
let currentPreviewId = null;

// Format bitrate to human readable
function formatBitrate(bps) {
    if (!bps || bps === 0) return '-';
    if (bps >= 1000000) {
        return (bps / 1000000).toFixed(2) + ' Mbps';
    } else if (bps >= 1000) {
        return (bps / 1000).toFixed(1) + ' Kbps';
    }
    return bps + ' bps';
}

// Fetch and update metrics for all inputs
async function fetchAllMetrics() {
    try {
        const response = await fetch('api/inputs.php?action=all_metrics');
        const data = await response.json();

        if (data.success && data.inputs) {
            for (const [inputId, metrics] of Object.entries(data.inputs)) {
                updateInputMetrics(inputId, metrics);
            }
        }
    } catch (e) {
        console.error('Failed to fetch metrics:', e);
    }
}

// Update metrics display for a single input
function updateInputMetrics(inputId, metrics) {
    const bitrateCell = document.getElementById(`bitrate-${inputId}`);
    const statusDot = document.getElementById(`status-${inputId}`);

    if (!bitrateCell) return;

    const videoSpan = bitrateCell.querySelector('.bitrate-video');
    const audioSpan = bitrateCell.querySelector('.bitrate-audio');

    if (metrics.status === 'offline' || !metrics.pids) {
        bitrateCell.classList.add('offline');
        if (videoSpan) videoSpan.textContent = '-';
        if (audioSpan) audioSpan.textContent = '-';
        return;
    }

    bitrateCell.classList.remove('offline');

    // Find video and audio PIDs
    let videoBitrate = 0;
    let audioBitrate = 0;

    if (metrics.pids) {
        for (const [pid, pidData] of Object.entries(metrics.pids)) {
            const bitrate = pidData.current_bitrate || 0;
            // Assume first PID is video (higher bitrate), rest are audio
            if (videoBitrate === 0 && bitrate > 500000) {
                videoBitrate = bitrate;
            } else {
                audioBitrate += bitrate;
            }
        }
    }

    if (videoSpan) videoSpan.textContent = formatBitrate(videoBitrate);
    if (audioSpan) audioSpan.textContent = formatBitrate(audioBitrate);

    // Update status dot based on bitrate
    if (statusDot && metrics.status === 'running') {
        if (videoBitrate > 0) {
            statusDot.className = 'status-dot status-running';
            statusDot.title = 'Running - receiving data';
        } else {
            statusDot.className = 'status-dot status-error';
            statusDot.title = 'Running - no data';
        }
    }
}

// Show preview modal
async function showPreview(inputId, inputName) {
    currentPreviewId = inputId;
    document.getElementById('previewInputId').value = inputId;
    document.getElementById('previewInputName').textContent = inputName;

    // Reset video player UI
    document.getElementById('videoPlaceholder').classList.remove('d-none');
    document.getElementById('videoPlaceholder').classList.add('d-flex');
    document.getElementById('videoLoading').classList.remove('d-flex');
    document.getElementById('videoLoading').classList.add('d-none');
    document.getElementById('previewVideo').classList.add('d-none');
    document.getElementById('videoStatusText').textContent = 'Starting preview...';

    // Initialize chart if needed
    if (!bitrateChart) {
        const ctx = document.getElementById('bitrateChart').getContext('2d');
        bitrateChart = new Chart(ctx, {
            type: 'line',
            data: {
                labels: [],
                datasets: [{
                    label: 'Video',
                    data: [],
                    borderColor: '#2563eb',
                    backgroundColor: 'rgba(37, 99, 235, 0.1)',
                    fill: true,
                    tension: 0.3,
                    pointRadius: 0
                }, {
                    label: 'Audio',
                    data: [],
                    borderColor: '#16a34a',
                    backgroundColor: 'rgba(22, 163, 74, 0.1)',
                    fill: true,
                    tension: 0.3,
                    pointRadius: 0
                }]
            },
            options: {
                responsive: true,
                maintainAspectRatio: false,
                interaction: {
                    intersect: false,
                    mode: 'index'
                },
                scales: {
                    y: {
                        beginAtZero: true,
                        ticks: {
                            callback: function(value) {
                                return formatBitrate(value);
                            }
                        }
                    },
                    x: {
                        display: true,
                        title: {
                            display: false
                        },
                        ticks: {
                            maxTicksLimit: 10,
                            maxRotation: 0
                        }
                    }
                },
                plugins: {
                    tooltip: {
                        callbacks: {
                            label: function(context) {
                                return context.dataset.label + ': ' + formatBitrate(context.raw);
                            }
                        }
                    }
                }
            }
        });
    }

    // Clear existing chart data
    bitrateChart.data.labels = [];
    bitrateChart.data.datasets[0].data = [];
    bitrateChart.data.datasets[1].data = [];
    bitrateChart.update();

    // Initialize A/V sync chart if needed
    if (!avsyncChart) {
        const avsyncCtx = document.getElementById('avsyncChart').getContext('2d');
        avsyncChart = new Chart(avsyncCtx, {
            type: 'line',
            data: {
                labels: [],
                datasets: [{
                    label: 'A→V',
                    data: [],
                    borderColor: '#8b5cf6',
                    backgroundColor: 'rgba(139, 92, 246, 0.1)',
                    fill: true,
                    tension: 0.3,
                    pointRadius: 2,
                    pointBackgroundColor: '#8b5cf6'
                }, {
                    label: 'V→A',
                    data: [],
                    borderColor: '#06b6d4',
                    backgroundColor: 'rgba(6, 182, 212, 0.1)',
                    fill: true,
                    tension: 0.3,
                    pointRadius: 2,
                    pointBackgroundColor: '#06b6d4'
                }]
            },
            options: {
                responsive: true,
                maintainAspectRatio: false,
                interaction: {
                    intersect: false,
                    mode: 'index'
                },
                scales: {
                    y: {
                        beginAtZero: true,
                        title: {
                            display: true,
                            text: 'Offset (ms)'
                        },
                        ticks: {
                            callback: function(value) {
                                return value + ' ms';
                            }
                        }
                    },
                    x: {
                        display: true,
                        ticks: {
                            maxTicksLimit: 8,
                            maxRotation: 0
                        }
                    }
                },
                plugins: {
                    tooltip: {
                        callbacks: {
                            label: function(context) {
                                return context.dataset.label + ': ' + context.raw.toFixed(1) + ' ms';
                            }
                        }
                    }
                }
            }
        });
    }

    // Clear existing A/V sync chart data
    avsyncChart.data.labels = [];
    avsyncChart.data.datasets[0].data = [];
    avsyncChart.data.datasets[1].data = [];
    avsyncChart.update();

    // Reset A/V sync display
    document.getElementById('avsyncStatus').className = 'badge bg-secondary';
    document.getElementById('avsyncStatus').textContent = 'Loading...';
    document.getElementById('avsyncA2V').textContent = '-';
    document.getElementById('avsyncV2A').textContent = '-';
    document.getElementById('avsyncCurrentStatus').textContent = '-';
    document.getElementById('avsyncSamples').textContent = '- samples';
    document.getElementById('avsyncLastUpdate').textContent = '-';

    // Show modal
    if (!previewModal) {
        previewModal = new bootstrap.Modal(document.getElementById('previewModal'));
    }
    previewModal.show();

    // Reset stream info panel
    document.getElementById('streamInfoLoading').style.display = 'block';
    document.getElementById('streamInfoContent').style.display = 'none';
    document.getElementById('streamInfoError').style.display = 'none';

    // Start player_preview
    await startPreview(inputId);

    // Load media info (don't await - let it load in background)
    loadMediaInfo(inputId);

    // Load historical bitrate data and A/V sync data in parallel
    await Promise.all([
        loadBitrateHistory(inputId),
        loadAVSyncHistory(inputId)
    ]);

    // Start live graph updates
    graphUpdateInterval = setInterval(() => updateBitrateGraph(inputId), 5000);

    // Start A/V sync updates (every 5 minutes = 300000ms)
    avsyncUpdateInterval = setInterval(() => loadAVSyncHistory(inputId), 300000);

    // Start keepalive (every 30 seconds)
    previewKeepaliveInterval = setInterval(() => sendPreviewKeepalive(inputId), 30000);

    // Clean up when modal closes
    document.getElementById('previewModal').addEventListener('hidden.bs.modal', function() {
        cleanupPreview();
    }, { once: true });
}

// Start preview and wait for it to be ready
async function startPreview(inputId) {
    try {
        // Show loading state
        document.getElementById('videoPlaceholder').classList.remove('d-flex');
        document.getElementById('videoPlaceholder').classList.add('d-none');
        document.getElementById('videoLoading').classList.remove('d-none');
        document.getElementById('videoLoading').classList.add('d-flex');
        document.getElementById('videoLoadingStatus').textContent = 'Starting preview...';

        // Start preview via API
        const startResponse = await fetch(`api/inputs.php?action=preview_start&id=${inputId}`, { method: 'POST' });
        const startData = await startResponse.json();

        if (!startData.success) {
            document.getElementById('videoLoadingStatus').textContent = 'Error: ' + (startData.error || 'Failed to start');
            return;
        }

        const playlistUrl = startData.playlist_url;
        document.getElementById('videoLoadingStatus').textContent = 'Waiting for segments...';

        // Poll for ready status
        let attempts = 0;
        const maxAttempts = 30; // 30 seconds max wait

        previewStatusInterval = setInterval(async () => {
            attempts++;
            try {
                const statusResponse = await fetch(`api/inputs.php?action=preview_status&id=${inputId}`);
                const statusData = await statusResponse.json();

                if (statusData.ready) {
                    clearInterval(previewStatusInterval);
                    previewStatusInterval = null;
                    document.getElementById('videoLoadingStatus').textContent = 'Loading player...';
                    initHlsPlayer(playlistUrl);
                } else if (statusData.running) {
                    document.getElementById('videoLoadingStatus').textContent = `Buffering... (${statusData.segments || 0} segments)`;
                } else if (attempts >= maxAttempts) {
                    clearInterval(previewStatusInterval);
                    previewStatusInterval = null;
                    document.getElementById('videoLoadingStatus').textContent = 'Timeout waiting for stream';
                }
            } catch (e) {
                console.error('Status check failed:', e);
            }
        }, 1000);

    } catch (e) {
        console.error('Failed to start preview:', e);
        document.getElementById('videoLoadingStatus').textContent = 'Error: ' + e.message;
    }
}

// Initialize HLS player
function initHlsPlayer(playlistUrl) {
    const video = document.getElementById('previewVideo');

    // Clean up existing player
    if (hlsPlayer) {
        hlsPlayer.destroy();
        hlsPlayer = null;
    }

    if (Hls.isSupported()) {
        hlsPlayer = new Hls();

        hlsPlayer.loadSource(playlistUrl);
        hlsPlayer.attachMedia(video);

        hlsPlayer.on(Hls.Events.MANIFEST_PARSED, function() {
            // Hide loading, show video
            document.getElementById('videoLoading').classList.remove('d-flex');
            document.getElementById('videoLoading').classList.add('d-none');
            video.classList.remove('d-none');
            video.play().catch(e => console.log('Autoplay blocked:', e));
        });

        hlsPlayer.on(Hls.Events.ERROR, function(event, data) {
            console.error('HLS error:', data);
            if (data.fatal) {
                document.getElementById('videoLoadingStatus').textContent = 'Playback error: ' + data.type;
            }
        });
    } else if (video.canPlayType('application/vnd.apple.mpegurl')) {
        // Safari native HLS
        video.src = playlistUrl;
        video.addEventListener('loadedmetadata', function() {
            document.getElementById('videoLoading').classList.remove('d-flex');
            document.getElementById('videoLoading').classList.add('d-none');
            video.classList.remove('d-none');
            video.play().catch(e => console.log('Autoplay blocked:', e));
        });
    } else {
        document.getElementById('videoLoadingStatus').textContent = 'HLS not supported in this browser';
    }
}

// Send keepalive to preview
async function sendPreviewKeepalive(inputId) {
    try {
        await fetch(`api/inputs.php?action=preview_keepalive&id=${inputId}`, { method: 'POST' });
    } catch (e) {
        console.error('Keepalive failed:', e);
    }
}

// Clean up preview resources
function cleanupPreview() {
    // Stop intervals
    if (graphUpdateInterval) {
        clearInterval(graphUpdateInterval);
        graphUpdateInterval = null;
    }
    if (avsyncUpdateInterval) {
        clearInterval(avsyncUpdateInterval);
        avsyncUpdateInterval = null;
    }
    if (previewKeepaliveInterval) {
        clearInterval(previewKeepaliveInterval);
        previewKeepaliveInterval = null;
    }
    if (previewStatusInterval) {
        clearInterval(previewStatusInterval);
        previewStatusInterval = null;
    }

    // Destroy HLS player
    if (hlsPlayer) {
        hlsPlayer.destroy();
        hlsPlayer = null;
    }

    // Reset video element
    const video = document.getElementById('previewVideo');
    if (video) {
        video.pause();
        video.src = '';
        video.classList.add('d-none');
    }

    currentPreviewId = null;
}

// Load media info via ffprobe
async function loadMediaInfo(inputId) {
    try {
        document.getElementById('streamInfoLoading').style.display = 'block';
        document.getElementById('streamInfoContent').style.display = 'none';
        document.getElementById('streamInfoError').style.display = 'none';

        const response = await fetch(`api/inputs.php?action=preview_media_info&id=${inputId}`);
        const data = await response.json();

        if (!data.success) {
            document.getElementById('streamInfoLoading').style.display = 'none';
            document.getElementById('streamInfoError').style.display = 'block';
            document.getElementById('streamInfoErrorText').textContent = data.error || 'Failed to load';
            return;
        }

        // Update video info
        if (data.video) {
            document.getElementById('infoVideoCodec').textContent = data.video.codec + (data.video.profile ? ` (${data.video.profile})` : '');
            document.getElementById('infoVideoRes').textContent = data.video.width && data.video.height ? `${data.video.width}x${data.video.height}` : '-';
            document.getElementById('infoVideoFps').textContent = data.video.fps ? `${Math.round(data.video.fps * 100) / 100} fps` : '-';
            document.getElementById('infoVideoProfile').textContent = data.video.pix_fmt || '-';
        } else {
            document.getElementById('infoVideoCodec').textContent = 'No video';
            document.getElementById('infoVideoRes').textContent = '-';
            document.getElementById('infoVideoFps').textContent = '-';
            document.getElementById('infoVideoProfile').textContent = '-';
        }

        // Update audio info
        const audioContainer = document.getElementById('infoAudioTracks');
        audioContainer.innerHTML = '';

        if (data.audio && data.audio.length > 0) {
            data.audio.forEach((track, idx) => {
                const trackDiv = document.createElement('div');
                trackDiv.className = 'mb-2 pb-2' + (idx < data.audio.length - 1 ? ' border-bottom' : '');
                trackDiv.innerHTML = `
                    <table class="table table-sm table-borderless mb-0">
                        <tr><td class="text-muted" style="width:40%">Track ${idx + 1}</td><td>${track.codec}${track.profile ? ' (' + track.profile + ')' : ''}</td></tr>
                        <tr><td class="text-muted">Channels</td><td>${track.channels}ch${track.channel_layout ? ' (' + track.channel_layout + ')' : ''}</td></tr>
                        <tr><td class="text-muted">Sample Rate</td><td>${track.sample_rate ? (track.sample_rate / 1000) + ' kHz' : '-'}</td></tr>
                        <tr><td class="text-muted">Language</td><td>${track.language || 'und'}</td></tr>
                    </table>
                `;
                audioContainer.appendChild(trackDiv);
            });
        } else {
            audioContainer.innerHTML = '<span class="text-muted">No audio tracks</span>';
        }

        // Show content
        document.getElementById('streamInfoLoading').style.display = 'none';
        document.getElementById('streamInfoContent').style.display = 'block';

    } catch (e) {
        console.error('Failed to load media info:', e);
        document.getElementById('streamInfoLoading').style.display = 'none';
        document.getElementById('streamInfoError').style.display = 'block';
        document.getElementById('streamInfoErrorText').textContent = e.message;
    }
}

// Refresh media info (called by button)
function refreshMediaInfo() {
    if (currentPreviewId) {
        loadMediaInfo(currentPreviewId);
    }
}

// Load historical bitrate data
async function loadBitrateHistory(inputId) {
    try {
        document.getElementById('graphStatus').className = 'badge bg-warning';
        document.getElementById('graphStatus').textContent = 'Loading history...';

        const response = await fetch(`api/inputs.php?action=metrics_history&id=${inputId}`);
        const data = await response.json();

        if (!data.success || !data.pids) {
            document.getElementById('graphStatus').className = 'badge bg-danger';
            document.getElementById('graphStatus').textContent = 'No history';
            return;
        }

        // Determine video and audio PIDs
        let videoPid = null, audioPids = [];
        for (const [pid, pidData] of Object.entries(data.pids)) {
            if (pidData.history && pidData.history.length > 0) {
                // Check last few samples to determine if video or audio
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
            document.getElementById('graphStatus').className = 'badge bg-warning';
            document.getElementById('graphStatus').textContent = 'No data yet';
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

        // Sort by timestamp and populate chart
        const sortedTimestamps = Array.from(timelineMap.keys()).sort((a, b) => a - b);

        // Limit to last 720 points (1 hour at 5-second intervals) for display
        const displayTimestamps = sortedTimestamps.slice(-720);

        for (const ts of displayTimestamps) {
            const date = new Date(ts * 1000);
            const label = date.toLocaleTimeString();
            const values = timelineMap.get(ts);

            bitrateChart.data.labels.push(label);
            bitrateChart.data.datasets[0].data.push(values.video);
            bitrateChart.data.datasets[1].data.push(values.audio);
        }

        bitrateChart.update();

        document.getElementById('graphStatus').className = 'badge bg-success';
        document.getElementById('graphStatus').textContent = 'Live';
        document.getElementById('graphLastUpdate').textContent = new Date().toLocaleTimeString();
        document.getElementById('graphOutputAddr').textContent = data.output_address || '-';

        // Update current stats from last sample
        if (displayTimestamps.length > 0) {
            const lastTs = displayTimestamps[displayTimestamps.length - 1];
            const lastValues = timelineMap.get(lastTs);
            document.getElementById('graphVideoBitrate').textContent = formatBitrate(lastValues.video);
            document.getElementById('graphAudioBitrate').textContent = formatBitrate(lastValues.audio);
            document.getElementById('graphVideoPid').textContent = videoPid || '-';
            document.getElementById('graphAudioPid').textContent = audioPids.join(', ') || '-';
        }

    } catch (e) {
        console.error('Failed to load history:', e);
        document.getElementById('graphStatus').className = 'badge bg-danger';
        document.getElementById('graphStatus').textContent = 'Error';
    }
}

// Update bitrate graph with current data
async function updateBitrateGraph(inputId) {
    try {
        const response = await fetch(`api/inputs.php?action=metrics&id=${inputId}`);
        const data = await response.json();

        if (!data.success) {
            document.getElementById('graphStatus').className = 'badge bg-danger';
            document.getElementById('graphStatus').textContent = 'Offline';
            return;
        }

        document.getElementById('graphStatus').className = 'badge bg-success';
        document.getElementById('graphStatus').textContent = 'Live';
        document.getElementById('graphLastUpdate').textContent = new Date().toLocaleTimeString();
        document.getElementById('graphOutputAddr').textContent = data.output_address || '-';

        // Process PIDs
        let videoPid = null, audioPids = [];
        let videoBitrate = 0, audioBitrate = 0;

        if (data.pids) {
            for (const [pid, pidData] of Object.entries(data.pids)) {
                const bitrate = pidData.current_bitrate || 0;
                if (bitrate > 500000 && !videoPid) {
                    videoPid = pid;
                    videoBitrate = bitrate;
                } else {
                    audioPids.push(pid);
                    audioBitrate += bitrate;
                }
            }
        }

        // Update stats display
        document.getElementById('graphVideoBitrate').textContent = formatBitrate(videoBitrate);
        document.getElementById('graphAudioBitrate').textContent = formatBitrate(audioBitrate);
        document.getElementById('graphVideoPid').textContent = videoPid || '-';
        document.getElementById('graphAudioPid').textContent = audioPids.join(', ') || '-';

        // Update chart
        const now = new Date().toLocaleTimeString();
        bitrateChart.data.labels.push(now);
        bitrateChart.data.datasets[0].data.push(videoBitrate);
        bitrateChart.data.datasets[1].data.push(audioBitrate);

        // Keep only last 720 points (1 hour at 5-second intervals)
        if (bitrateChart.data.labels.length > 720) {
            bitrateChart.data.labels.shift();
            bitrateChart.data.datasets[0].data.shift();
            bitrateChart.data.datasets[1].data.shift();
        }

        bitrateChart.update('none');
    } catch (e) {
        console.error('Failed to update graph:', e);
    }
}

// ============ A/V Sync Functions ============

// Load A/V sync history from cari-avsync service
async function loadAVSyncHistory(inputId) {
    try {
        document.getElementById('avsyncStatus').className = 'badge bg-secondary';
        document.getElementById('avsyncStatus').textContent = 'Loading...';

        // Fetch from cari-avsync API
        const response = await fetch(`http://${window.location.hostname}:8082/history/${inputId}`);

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
            document.getElementById('avsyncSamples').textContent = 'Input not running';
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
            document.getElementById('avsyncLastUpdate').textContent = displayData.timestamp;

            // Update status badge
            const badge = document.getElementById('avsyncStatus');
            if (status === 'OK') {
                badge.className = 'badge avsync-ok';
                badge.textContent = 'OK';
            } else if (status === 'WARNING') {
                badge.className = 'badge avsync-warning';
                badge.textContent = 'WARNING';
            } else if (status === 'ERROR') {
                badge.className = 'badge avsync-error';
                badge.textContent = 'ERROR';
            } else {
                badge.className = 'badge bg-secondary';
                badge.textContent = 'No Data';
            }
        } else {
            // No data available at all
            document.getElementById('avsyncStatus').className = 'badge bg-secondary';
            document.getElementById('avsyncStatus').textContent = 'No Data';
            document.getElementById('avsyncA2V').textContent = '-';
            document.getElementById('avsyncV2A').textContent = '-';
            document.getElementById('avsyncCurrentStatus').textContent = '-';
            document.getElementById('avsyncSamples').textContent = 'No measurements yet';
        }

        // Update chart with history
        if (data.history && data.history.length > 0) {
            avsyncChart.data.labels = [];
            avsyncChart.data.datasets[0].data = [];
            avsyncChart.data.datasets[1].data = [];

            for (const entry of data.history) {
                // Format timestamp for display (HH:MM)
                const ts = entry.timestamp;
                const timePart = ts.includes(' ') ? ts.split(' ')[1] : ts;
                const shortTime = timePart.substring(0, 5); // HH:MM

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

// Start metrics polling on page load
document.addEventListener('DOMContentLoaded', function() {
    // Fetch metrics immediately and then every 5 seconds
    fetchAllMetrics();
    metricsInterval = setInterval(fetchAllMetrics, 5000);
});

// Clean up on page unload
window.addEventListener('beforeunload', function() {
    if (metricsInterval) clearInterval(metricsInterval);
    if (graphUpdateInterval) clearInterval(graphUpdateInterval);
    if (avsyncUpdateInterval) clearInterval(avsyncUpdateInterval);
});
</script>

<?php include __DIR__ . '/../templates/footer.php'; ?>
