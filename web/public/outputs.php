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
        <h2><i class="bi bi-upload me-2"></i>Output Destinations</h2>
        <button class="btn btn-primary" data-bs-toggle="modal" data-bs-target="#addOutputModal">
            <i class="bi bi-plus-lg me-1"></i>Add Output
        </button>
    </div>

    <div class="row" id="outputs-grid">
        <?php if (empty($outputs)): ?>
        <div class="col-12">
            <div class="card">
                <div class="card-body text-center py-5">
                    <i class="bi bi-upload text-muted" style="font-size: 3rem;"></i>
                    <h5 class="mt-3">No Output Destinations</h5>
                    <p class="text-muted">Create an output to send your streams.</p>
                    <button class="btn btn-primary" data-bs-toggle="modal" data-bs-target="#addOutputModal">
                        <i class="bi bi-plus-lg me-1"></i>Add Output
                    </button>
                </div>
            </div>
        </div>
        <?php else: ?>
        <?php foreach ($outputs as $output): ?>
        <div class="col-xl-4 col-md-6 mb-4">
            <div class="card service-card h-100">
                <div class="card-header d-flex justify-content-between align-items-center">
                    <h6 class="mb-0"><?php echo htmlspecialchars($output['name']); ?></h6>
                    <span class="badge bg-<?php echo $output['status'] === 'running' ? 'success' : 'secondary'; ?>">
                        <?php echo ucfirst($output['status']); ?>
                    </span>
                </div>
                <div class="card-body">
                    <div class="mb-3">
                        <small class="text-muted">Type</small>
                        <div><span class="badge bg-info"><?php echo htmlspecialchars($output['type'] ?? 'UDP'); ?></span></div>
                    </div>
                    <div class="mb-3">
                        <small class="text-muted">Destination</small>
                        <div class="text-truncate"><?php echo htmlspecialchars($output['destination'] ?? 'Not configured'); ?></div>
                    </div>
                    <div class="row">
                        <div class="col-6">
                            <small class="text-muted">Bitrate</small>
                            <div class="fw-bold"><?php echo format_bitrate($output['bitrate'] ?? 0); ?></div>
                        </div>
                        <div class="col-6">
                            <small class="text-muted">Packets Sent</small>
                            <div class="fw-bold"><?php echo number_format($output['packets'] ?? 0); ?></div>
                        </div>
                    </div>
                </div>
                <div class="card-footer bg-transparent">
                    <div class="btn-group w-100">
                        <?php if ($output['status'] === 'running'): ?>
                        <button class="btn btn-outline-warning btn-sm" onclick="stopService('outputs', '<?php echo $output['id']; ?>')">
                            <i class="bi bi-stop-fill"></i> Stop
                        </button>
                        <?php else: ?>
                        <button class="btn btn-outline-success btn-sm" onclick="startService('outputs', '<?php echo $output['id']; ?>')">
                            <i class="bi bi-play-fill"></i> Start
                        </button>
                        <?php endif; ?>
                        <button class="btn btn-outline-secondary btn-sm" onclick="editService('outputs', '<?php echo $output['id']; ?>')">
                            <i class="bi bi-gear"></i> Edit
                        </button>
                        <button class="btn btn-outline-danger btn-sm" onclick="deleteService('outputs', '<?php echo $output['id']; ?>')">
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

<!-- Add Output Modal -->
<div class="modal fade" id="addOutputModal" tabindex="-1">
    <div class="modal-dialog modal-lg">
        <div class="modal-content">
            <div class="modal-header">
                <h5 class="modal-title"><i class="bi bi-plus-lg me-2"></i>Add Output</h5>
                <button type="button" class="btn-close" data-bs-dismiss="modal"></button>
            </div>
            <form id="addOutputForm" action="api/outputs.php" method="POST">
                <div class="modal-body">
                    <div class="row">
                        <div class="col-md-6 mb-3">
                            <label class="form-label">Name</label>
                            <input type="text" class="form-control" name="name" required>
                        </div>
                        <div class="col-md-6 mb-3">
                            <label class="form-label">ID</label>
                            <input type="text" class="form-control" name="id" required pattern="[a-z0-9-]+">
                        </div>
                    </div>
                    <div class="row">
                        <div class="col-md-6 mb-3">
                            <label class="form-label">Output Type</label>
                            <select class="form-select" name="type" id="outputType" onchange="updateOutputFields()">
                                <option value="udp">UDP Multicast</option>
                                <option value="srt">SRT</option>
                                <option value="rtmp">RTMP</option>
                                <option value="hls">HLS</option>
                            </select>
                        </div>
                        <div class="col-md-6 mb-3">
                            <label class="form-label">Input Buffer</label>
                            <input type="text" class="form-control" name="input_buffer" required>
                        </div>
                    </div>

                    <div id="out-udp-fields">
                        <div class="row">
                            <div class="col-md-6 mb-3">
                                <label class="form-label">Destination Address</label>
                                <input type="text" class="form-control" name="udp_address" placeholder="239.1.1.1">
                            </div>
                            <div class="col-md-3 mb-3">
                                <label class="form-label">Port</label>
                                <input type="number" class="form-control" name="udp_port" placeholder="5000">
                            </div>
                            <div class="col-md-3 mb-3">
                                <label class="form-label">TTL</label>
                                <input type="number" class="form-control" name="udp_ttl" value="64">
                            </div>
                        </div>
                    </div>

                    <div id="out-srt-fields" style="display:none;">
                        <div class="row">
                            <div class="col-md-4 mb-3">
                                <label class="form-label">Mode</label>
                                <select class="form-select" name="srt_mode">
                                    <option value="caller">Caller</option>
                                    <option value="listener">Listener</option>
                                </select>
                            </div>
                            <div class="col-md-4 mb-3">
                                <label class="form-label">Address</label>
                                <input type="text" class="form-control" name="srt_address">
                            </div>
                            <div class="col-md-4 mb-3">
                                <label class="form-label">Port</label>
                                <input type="number" class="form-control" name="srt_port">
                            </div>
                        </div>
                    </div>

                    <div id="out-rtmp-fields" style="display:none;">
                        <div class="mb-3">
                            <label class="form-label">RTMP URL</label>
                            <input type="text" class="form-control" name="rtmp_url" placeholder="rtmp://server/app/stream">
                        </div>
                    </div>

                    <div id="out-hls-fields" style="display:none;">
                        <div class="row">
                            <div class="col-md-6 mb-3">
                                <label class="form-label">Output Directory</label>
                                <input type="text" class="form-control" name="hls_path" placeholder="/var/www/hls">
                            </div>
                            <div class="col-md-3 mb-3">
                                <label class="form-label">Segment Duration</label>
                                <input type="number" class="form-control" name="hls_segment" value="4">
                            </div>
                            <div class="col-md-3 mb-3">
                                <label class="form-label">Playlist Size</label>
                                <input type="number" class="form-control" name="hls_playlist" value="5">
                            </div>
                        </div>
                    </div>
                </div>
                <div class="modal-footer">
                    <button type="button" class="btn btn-secondary" data-bs-dismiss="modal">Cancel</button>
                    <button type="submit" class="btn btn-primary">Create Output</button>
                </div>
            </form>
        </div>
    </div>
</div>

<script>
function updateOutputFields() {
    const type = document.getElementById('outputType').value;
    document.getElementById('out-udp-fields').style.display = type === 'udp' ? 'block' : 'none';
    document.getElementById('out-srt-fields').style.display = type === 'srt' ? 'block' : 'none';
    document.getElementById('out-rtmp-fields').style.display = type === 'rtmp' ? 'block' : 'none';
    document.getElementById('out-hls-fields').style.display = type === 'hls' ? 'block' : 'none';
}
function startService(type, id) {
    fetch(`api/${type}.php?action=start&id=${id}`, { method: 'POST' })
        .then(r => r.json()).then(data => { if (data.success) location.reload(); else alert(data.error); });
}
function stopService(type, id) {
    fetch(`api/${type}.php?action=stop&id=${id}`, { method: 'POST' })
        .then(r => r.json()).then(data => { if (data.success) location.reload(); else alert(data.error); });
}
function deleteService(type, id) {
    if (confirm('Delete this output?')) {
        fetch(`api/${type}.php?action=delete&id=${id}`, { method: 'POST' })
            .then(r => r.json()).then(data => { if (data.success) location.reload(); else alert(data.error); });
    }
}
function editService(type, id) { window.location.href = `${type}-edit.php?id=${id}`; }
</script>

<?php include __DIR__ . '/../templates/footer.php'; ?>
