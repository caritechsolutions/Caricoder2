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
$page_title = 'Transcoders';
include __DIR__ . '/../templates/header.php';
?>

<div class="container-fluid py-4">
    <div class="d-flex justify-content-between align-items-center mb-4">
        <h2><i class="bi bi-arrow-repeat me-2"></i>Transcoders</h2>
        <a href="transcoders-edit.php" class="btn btn-primary">
            <i class="bi bi-plus-lg me-1"></i>Add Transcoder
        </a>
    </div>

    <div class="row" id="transcoders-grid">
        <?php if (empty($transcoders)): ?>
        <div class="col-12">
            <div class="card">
                <div class="card-body text-center py-5">
                    <i class="bi bi-arrow-repeat text-muted" style="font-size: 3rem;"></i>
                    <h5 class="mt-3">No Transcoders</h5>
                    <p class="text-muted">Create a transcoder to convert video/audio formats.</p>
                    <a href="transcoders-edit.php" class="btn btn-primary">
                        <i class="bi bi-plus-lg me-1"></i>Add Transcoder
                    </a>
                </div>
            </div>
        </div>
        <?php else: ?>
        <?php foreach ($transcoders as $transcoder): ?>
        <div class="col-xl-4 col-md-6 mb-4">
            <div class="card service-card h-100">
                <div class="card-header d-flex justify-content-between align-items-center">
                    <h6 class="mb-0"><?php echo htmlspecialchars($transcoder['name']); ?></h6>
                    <span class="badge bg-<?php echo $transcoder['status'] === 'running' ? 'success' : 'secondary'; ?>">
                        <?php echo ucfirst($transcoder['status']); ?>
                    </span>
                </div>
                <div class="card-body">
                    <div class="row mb-3">
                        <div class="col-6">
                            <small class="text-muted">Video Codec</small>
                            <div><span class="badge bg-info"><?php echo htmlspecialchars($transcoder['video_codec'] ?? 'H.264'); ?></span></div>
                        </div>
                        <div class="col-6">
                            <small class="text-muted">Audio Codec</small>
                            <div><span class="badge bg-secondary"><?php echo htmlspecialchars($transcoder['audio_codec'] ?? 'AAC'); ?></span></div>
                        </div>
                    </div>
                    <div class="row mb-3">
                        <div class="col-6">
                            <small class="text-muted">Video Bitrate</small>
                            <div class="fw-bold"><?php echo format_bitrate($transcoder['video_bitrate'] ?? 0); ?></div>
                        </div>
                        <div class="col-6">
                            <small class="text-muted">Resolution</small>
                            <div class="fw-bold"><?php echo htmlspecialchars($transcoder['resolution'] ?? '1920x1080'); ?></div>
                        </div>
                    </div>
                    <div class="mb-2">
                        <small class="text-muted">Input Buffer</small>
                        <div class="text-truncate"><?php echo htmlspecialchars($transcoder['input_buffer'] ?? 'Not set'); ?></div>
                    </div>
                </div>
                <div class="card-footer bg-transparent">
                    <div class="btn-group w-100">
                        <?php if ($transcoder['status'] === 'running'): ?>
                        <button class="btn btn-outline-warning btn-sm" onclick="stopService('transcoders', '<?php echo $transcoder['id']; ?>')">
                            <i class="bi bi-stop-fill"></i> Stop
                        </button>
                        <?php else: ?>
                        <button class="btn btn-outline-success btn-sm" onclick="startService('transcoders', '<?php echo $transcoder['id']; ?>')">
                            <i class="bi bi-play-fill"></i> Start
                        </button>
                        <?php endif; ?>
                        <button class="btn btn-outline-secondary btn-sm" onclick="editService('transcoders', '<?php echo $transcoder['id']; ?>')">
                            <i class="bi bi-gear"></i> Edit
                        </button>
                        <button class="btn btn-outline-danger btn-sm" onclick="deleteService('transcoders', '<?php echo $transcoder['id']; ?>')">
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

<script>
function startService(type, id) {
    fetch(`api/${type}.php?action=start&id=${id}`, { method: 'POST' })
        .then(r => r.json())
        .then(data => { if (data.success) location.reload(); else alert(data.error); });
}
function stopService(type, id) {
    fetch(`api/${type}.php?action=stop&id=${id}`, { method: 'POST' })
        .then(r => r.json())
        .then(data => { if (data.success) location.reload(); else alert(data.error); });
}
function deleteService(type, id) {
    if (confirm('Delete this transcoder?')) {
        fetch(`api/${type}.php?action=delete&id=${id}`, { method: 'POST' })
            .then(r => r.json())
            .then(data => { if (data.success) location.reload(); else alert(data.error); });
    }
}
function editService(type, id) { window.location.href = `${type}-edit.php?id=${id}`; }
</script>

<?php include __DIR__ . '/../templates/footer.php'; ?>
