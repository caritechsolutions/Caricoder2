<?php
/**
 * CariTranscoder - Muxers Management
 * Copyright (c) 2024 CariTech Solutions
 */

define('CARITRANS', true);
require_once __DIR__ . '/../includes/config.php';
require_once __DIR__ . '/../includes/auth.php';
require_once __DIR__ . '/../includes/functions.php';

auth_require_login();

$muxers = get_service_list('muxers');
$page_title = 'Muxers';
include __DIR__ . '/../templates/header.php';
?>

<div class="container-fluid py-4">
    <div class="d-flex justify-content-between align-items-center mb-4">
        <h2><i class="bi bi-collection me-2"></i>Muxers</h2>
        <button class="btn btn-primary" data-bs-toggle="modal" data-bs-target="#addMuxerModal">
            <i class="bi bi-plus-lg me-1"></i>Add Muxer
        </button>
    </div>

    <div class="row" id="muxers-grid">
        <?php if (empty($muxers)): ?>
        <div class="col-12">
            <div class="card">
                <div class="card-body text-center py-5">
                    <i class="bi bi-collection text-muted" style="font-size: 3rem;"></i>
                    <h5 class="mt-3">No Muxers</h5>
                    <p class="text-muted">Create a muxer to combine multiple streams into MPTS.</p>
                    <button class="btn btn-primary" data-bs-toggle="modal" data-bs-target="#addMuxerModal">
                        <i class="bi bi-plus-lg me-1"></i>Add Muxer
                    </button>
                </div>
            </div>
        </div>
        <?php else: ?>
        <?php foreach ($muxers as $muxer): ?>
        <div class="col-xl-4 col-md-6 mb-4">
            <div class="card service-card h-100">
                <div class="card-header d-flex justify-content-between align-items-center">
                    <h6 class="mb-0"><?php echo htmlspecialchars($muxer['name']); ?></h6>
                    <span class="badge bg-<?php echo $muxer['status'] === 'running' ? 'success' : 'secondary'; ?>">
                        <?php echo ucfirst($muxer['status']); ?>
                    </span>
                </div>
                <div class="card-body">
                    <div class="mb-3">
                        <small class="text-muted">Mode</small>
                        <div><span class="badge bg-warning"><?php echo htmlspecialchars($muxer['mode'] ?? 'MPTS'); ?></span></div>
                    </div>
                    <div class="mb-3">
                        <small class="text-muted">Inputs</small>
                        <div class="fw-bold"><?php echo $muxer['input_count'] ?? 0; ?> streams</div>
                    </div>
                    <div class="row">
                        <div class="col-6">
                            <small class="text-muted">Output Bitrate</small>
                            <div class="fw-bold"><?php echo format_bitrate($muxer['bitrate'] ?? 0); ?></div>
                        </div>
                        <div class="col-6">
                            <small class="text-muted">TS Rate</small>
                            <div class="fw-bold"><?php echo format_bitrate($muxer['ts_rate'] ?? 0); ?></div>
                        </div>
                    </div>
                </div>
                <div class="card-footer bg-transparent">
                    <div class="btn-group w-100">
                        <?php if ($muxer['status'] === 'running'): ?>
                        <button class="btn btn-outline-warning btn-sm" onclick="stopService('muxers', '<?php echo $muxer['id']; ?>')">
                            <i class="bi bi-stop-fill"></i> Stop
                        </button>
                        <?php else: ?>
                        <button class="btn btn-outline-success btn-sm" onclick="startService('muxers', '<?php echo $muxer['id']; ?>')">
                            <i class="bi bi-play-fill"></i> Start
                        </button>
                        <?php endif; ?>
                        <button class="btn btn-outline-secondary btn-sm" onclick="editService('muxers', '<?php echo $muxer['id']; ?>')">
                            <i class="bi bi-gear"></i> Edit
                        </button>
                        <button class="btn btn-outline-danger btn-sm" onclick="deleteService('muxers', '<?php echo $muxer['id']; ?>')">
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

<!-- Add Muxer Modal -->
<div class="modal fade" id="addMuxerModal" tabindex="-1">
    <div class="modal-dialog modal-lg">
        <div class="modal-content">
            <div class="modal-header">
                <h5 class="modal-title"><i class="bi bi-plus-lg me-2"></i>Add Muxer</h5>
                <button type="button" class="btn-close" data-bs-dismiss="modal"></button>
            </div>
            <form id="addMuxerForm" action="api/muxers.php" method="POST">
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
                            <label class="form-label">Mode</label>
                            <select class="form-select" name="mode">
                                <option value="mpts">MPTS (Multiple Programs)</option>
                                <option value="spts">SPTS (Single Program)</option>
                            </select>
                        </div>
                        <div class="col-md-6 mb-3">
                            <label class="form-label">Output Buffer</label>
                            <input type="text" class="form-control" name="output_buffer" required>
                        </div>
                    </div>
                    <div class="row">
                        <div class="col-md-6 mb-3">
                            <label class="form-label">TS Bitrate (bps)</label>
                            <input type="number" class="form-control" name="ts_bitrate" value="38000000">
                        </div>
                        <div class="col-md-6 mb-3">
                            <label class="form-label">Network ID</label>
                            <input type="number" class="form-control" name="network_id" value="1">
                        </div>
                    </div>
                    <hr>
                    <h6>Input Streams</h6>
                    <div id="input-streams">
                        <div class="input-stream-row mb-2">
                            <div class="row">
                                <div class="col-md-5">
                                    <input type="text" class="form-control" name="inputs[0][buffer]" placeholder="Input buffer name">
                                </div>
                                <div class="col-md-3">
                                    <input type="number" class="form-control" name="inputs[0][program]" placeholder="Program #" value="1">
                                </div>
                                <div class="col-md-3">
                                    <input type="number" class="form-control" name="inputs[0][pmt_pid]" placeholder="PMT PID" value="256">
                                </div>
                                <div class="col-md-1">
                                    <button type="button" class="btn btn-outline-danger" onclick="this.closest('.input-stream-row').remove()">
                                        <i class="bi bi-x"></i>
                                    </button>
                                </div>
                            </div>
                        </div>
                    </div>
                    <button type="button" class="btn btn-outline-secondary btn-sm" onclick="addInputStream()">
                        <i class="bi bi-plus"></i> Add Input Stream
                    </button>
                </div>
                <div class="modal-footer">
                    <button type="button" class="btn btn-secondary" data-bs-dismiss="modal">Cancel</button>
                    <button type="submit" class="btn btn-primary">Create Muxer</button>
                </div>
            </form>
        </div>
    </div>
</div>

<script>
let inputCount = 1;
function addInputStream() {
    const container = document.getElementById('input-streams');
    const html = `<div class="input-stream-row mb-2">
        <div class="row">
            <div class="col-md-5"><input type="text" class="form-control" name="inputs[${inputCount}][buffer]" placeholder="Input buffer name"></div>
            <div class="col-md-3"><input type="number" class="form-control" name="inputs[${inputCount}][program]" placeholder="Program #"></div>
            <div class="col-md-3"><input type="number" class="form-control" name="inputs[${inputCount}][pmt_pid]" placeholder="PMT PID"></div>
            <div class="col-md-1"><button type="button" class="btn btn-outline-danger" onclick="this.closest('.input-stream-row').remove()"><i class="bi bi-x"></i></button></div>
        </div>
    </div>`;
    container.insertAdjacentHTML('beforeend', html);
    inputCount++;
}
function startService(type, id) {
    fetch(`api/${type}.php?action=start&id=${id}`, { method: 'POST' }).then(r => r.json()).then(d => { if (d.success) location.reload(); else alert(d.error); });
}
function stopService(type, id) {
    fetch(`api/${type}.php?action=stop&id=${id}`, { method: 'POST' }).then(r => r.json()).then(d => { if (d.success) location.reload(); else alert(d.error); });
}
function deleteService(type, id) {
    if (confirm('Delete this muxer?')) fetch(`api/${type}.php?action=delete&id=${id}`, { method: 'POST' }).then(r => r.json()).then(d => { if (d.success) location.reload(); else alert(d.error); });
}
function editService(type, id) { window.location.href = `${type}-edit.php?id=${id}`; }
</script>

<?php include __DIR__ . '/../templates/footer.php'; ?>
