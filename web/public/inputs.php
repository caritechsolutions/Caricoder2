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
                        <div><span class="badge bg-primary"><?php echo htmlspecialchars($input['type'] ?? 'UDP'); ?></span></div>
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
                        <button class="btn btn-outline-secondary btn-sm" onclick="editService('inputs', '<?php echo $input['id']; ?>')">
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
    <div class="modal-dialog modal-lg">
        <div class="modal-content">
            <div class="modal-header">
                <h5 class="modal-title"><i class="bi bi-plus-lg me-2"></i>Add Input Source</h5>
                <button type="button" class="btn-close" data-bs-dismiss="modal"></button>
            </div>
            <form id="addInputForm" action="api/inputs.php" method="POST">
                <div class="modal-body">
                    <div class="row">
                        <div class="col-md-6 mb-3">
                            <label class="form-label">Name</label>
                            <input type="text" class="form-control" name="name" required placeholder="My Input">
                        </div>
                        <div class="col-md-6 mb-3">
                            <label class="form-label">ID</label>
                            <input type="text" class="form-control" name="id" required placeholder="input-001" pattern="[a-z0-9-]+">
                            <small class="text-muted">Lowercase letters, numbers, and hyphens only</small>
                        </div>
                    </div>
                    <div class="row">
                        <div class="col-md-6 mb-3">
                            <label class="form-label">Input Type</label>
                            <select class="form-select" name="type" id="inputType" onchange="updateInputFields()">
                                <option value="udp">UDP Multicast</option>
                                <option value="srt">SRT</option>
                                <option value="rtmp">RTMP</option>
                                <option value="hls">HLS</option>
                                <option value="file">File</option>
                            </select>
                        </div>
                        <div class="col-md-6 mb-3">
                            <label class="form-label">Buffer Name</label>
                            <input type="text" class="form-control" name="buffer" placeholder="buffer-input-001">
                        </div>
                    </div>

                    <!-- UDP Fields -->
                    <div id="udp-fields">
                        <div class="row">
                            <div class="col-md-8 mb-3">
                                <label class="form-label">Multicast Address</label>
                                <input type="text" class="form-control" name="udp_address" placeholder="239.1.1.1">
                            </div>
                            <div class="col-md-4 mb-3">
                                <label class="form-label">Port</label>
                                <input type="number" class="form-control" name="udp_port" placeholder="5000">
                            </div>
                        </div>
                    </div>

                    <!-- SRT Fields -->
                    <div id="srt-fields" style="display:none;">
                        <div class="row">
                            <div class="col-md-4 mb-3">
                                <label class="form-label">Mode</label>
                                <select class="form-select" name="srt_mode">
                                    <option value="listener">Listener</option>
                                    <option value="caller">Caller</option>
                                </select>
                            </div>
                            <div class="col-md-4 mb-3">
                                <label class="form-label">Address</label>
                                <input type="text" class="form-control" name="srt_address" placeholder="0.0.0.0">
                            </div>
                            <div class="col-md-4 mb-3">
                                <label class="form-label">Port</label>
                                <input type="number" class="form-control" name="srt_port" placeholder="9000">
                            </div>
                        </div>
                        <div class="row">
                            <div class="col-md-6 mb-3">
                                <label class="form-label">Latency (ms)</label>
                                <input type="number" class="form-control" name="srt_latency" value="200">
                            </div>
                            <div class="col-md-6 mb-3">
                                <label class="form-label">Passphrase (optional)</label>
                                <input type="password" class="form-control" name="srt_passphrase" placeholder="Encryption key">
                            </div>
                        </div>
                    </div>

                    <!-- RTMP Fields -->
                    <div id="rtmp-fields" style="display:none;">
                        <div class="mb-3">
                            <label class="form-label">RTMP URL</label>
                            <input type="text" class="form-control" name="rtmp_url" placeholder="rtmp://server/app/stream">
                        </div>
                    </div>

                    <!-- HLS Fields -->
                    <div id="hls-fields" style="display:none;">
                        <div class="mb-3">
                            <label class="form-label">HLS URL</label>
                            <input type="text" class="form-control" name="hls_url" placeholder="https://example.com/stream.m3u8">
                        </div>
                    </div>

                    <!-- File Fields -->
                    <div id="file-fields" style="display:none;">
                        <div class="row">
                            <div class="col-md-8 mb-3">
                                <label class="form-label">File Path</label>
                                <input type="text" class="form-control" name="file_path" placeholder="/path/to/file.ts">
                            </div>
                            <div class="col-md-4 mb-3">
                                <label class="form-label">Loop</label>
                                <select class="form-select" name="file_loop">
                                    <option value="1">Yes</option>
                                    <option value="0">No</option>
                                </select>
                            </div>
                        </div>
                    </div>
                </div>
                <div class="modal-footer">
                    <button type="button" class="btn btn-secondary" data-bs-dismiss="modal">Cancel</button>
                    <button type="submit" class="btn btn-primary">Create Input</button>
                </div>
            </form>
        </div>
    </div>
</div>

<script>
function updateInputFields() {
    const type = document.getElementById('inputType').value;
    document.getElementById('udp-fields').style.display = type === 'udp' ? 'block' : 'none';
    document.getElementById('srt-fields').style.display = type === 'srt' ? 'block' : 'none';
    document.getElementById('rtmp-fields').style.display = type === 'rtmp' ? 'block' : 'none';
    document.getElementById('hls-fields').style.display = type === 'hls' ? 'block' : 'none';
    document.getElementById('file-fields').style.display = type === 'file' ? 'block' : 'none';
}

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
    if (confirm('Are you sure you want to delete this service?')) {
        fetch(`api/${type}.php?action=delete&id=${id}`, { method: 'POST' })
            .then(r => r.json())
            .then(data => {
                if (data.success) location.reload();
                else alert(data.error || 'Failed to delete service');
            });
    }
}

function editService(type, id) {
    window.location.href = `${type}-edit.php?id=${id}`;
}
</script>

<?php include __DIR__ . '/../templates/footer.php'; ?>
