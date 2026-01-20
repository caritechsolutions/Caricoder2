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
                        <div>
                            <?php
                            $type = $output['type'] ?? 'UDP';
                            $type_badge = 'info';
                            $type_icon = 'bi-broadcast';
                            if ($type === 'srt') {
                                $type_badge = 'primary';
                                $type_icon = 'bi-shield-lock';
                            } elseif ($type === 'hls') {
                                $type_badge = 'warning';
                                $type_icon = 'bi-collection-play';
                            }
                            ?>
                            <span class="badge bg-<?php echo $type_badge; ?>">
                                <i class="bi <?php echo $type_icon; ?> me-1"></i><?php echo strtoupper(htmlspecialchars($type)); ?>
                            </span>
                            <?php if ($type === 'srt' && ($output['srt_mode'] ?? '') === 'listener'): ?>
                            <span class="badge bg-success ms-1" title="Multiple clients can connect">
                                <i class="bi bi-people-fill"></i> 1:N
                            </span>
                            <?php endif; ?>
                        </div>
                    </div>
                    <div class="mb-3">
                        <small class="text-muted">Destination</small>
                        <div class="text-truncate font-monospace small">
                            <?php
                            $dest = 'Not configured';
                            if ($type === 'udp') {
                                $addr = $output['destination']['address'] ?? $output['udp_address'] ?? '';
                                $port = $output['destination']['port'] ?? $output['udp_port'] ?? '';
                                if ($addr && $port) $dest = "udp://{$addr}:{$port}";
                            } elseif ($type === 'srt') {
                                $mode = $output['destination_srt']['mode'] ?? $output['srt_mode'] ?? 'listener';
                                $addr = $output['destination_srt']['listen_address'] ?? '0.0.0.0';
                                $port = $output['destination_srt']['listen_port'] ?? $output['srt_port'] ?? '';
                                if ($port) $dest = "srt://{$addr}:{$port} ({$mode})";
                            } elseif ($type === 'hls') {
                                $dest = $output['destination_hls']['output_dir'] ?? $output['hls_path'] ?? 'Not set';
                            }
                            echo htmlspecialchars($dest);
                            ?>
                        </div>
                    </div>
                    <div class="mb-3">
                        <small class="text-muted">Input Buffer</small>
                        <div class="text-truncate"><?php echo htmlspecialchars($output['input']['buffer_name'] ?? $output['input_buffer'] ?? 'Not set'); ?></div>
                    </div>
                    <?php if ($type === 'srt' && $output['status'] === 'running'): ?>
                    <div class="row">
                        <div class="col-6">
                            <small class="text-muted">Connected</small>
                            <div class="fw-bold" id="clients-<?php echo $output['id']; ?>">-</div>
                        </div>
                        <div class="col-6">
                            <small class="text-muted">Latency</small>
                            <div class="fw-bold"><?php echo htmlspecialchars($output['destination_srt']['latency'] ?? '120'); ?> ms</div>
                        </div>
                    </div>
                    <?php endif; ?>
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
            <form id="addOutputForm">
                <div class="modal-body">
                    <div class="row">
                        <div class="col-md-6 mb-3">
                            <label class="form-label">Name <span class="text-danger">*</span></label>
                            <input type="text" class="form-control" name="name" id="outputName" required
                                   placeholder="e.g., Main SRT Output">
                        </div>
                        <div class="col-md-6 mb-3">
                            <label class="form-label">ID</label>
                            <input type="text" class="form-control" name="id" id="outputId"
                                   pattern="[a-z0-9-]+" placeholder="auto-generated">
                            <small class="text-muted">Leave blank to auto-generate from name</small>
                        </div>
                    </div>
                    <div class="row">
                        <div class="col-md-6 mb-3">
                            <label class="form-label">Output Type <span class="text-danger">*</span></label>
                            <select class="form-select" name="type" id="outputType" onchange="updateOutputFields()">
                                <option value="udp">UDP Multicast</option>
                                <option value="srt" selected>SRT (One-to-Many)</option>
                                <option value="hls">HLS</option>
                            </select>
                        </div>
                        <div class="col-md-6 mb-3">
                            <label class="form-label">Input Buffer <span class="text-danger">*</span></label>
                            <input type="text" class="form-control" name="input_buffer" required
                                   placeholder="e.g., mux-001-out">
                            <small class="text-muted">Ring buffer name from muxer or transcoder</small>
                        </div>
                    </div>

                    <!-- UDP Fields -->
                    <div id="out-udp-fields" style="display:none;">
                        <hr>
                        <h6><i class="bi bi-broadcast me-2"></i>UDP Destination</h6>
                        <div class="row">
                            <div class="col-md-6 mb-3">
                                <label class="form-label">Multicast Address</label>
                                <input type="text" class="form-control" name="udp_address"
                                       placeholder="239.1.1.1" value="239.1.1.1">
                            </div>
                            <div class="col-md-3 mb-3">
                                <label class="form-label">Port</label>
                                <input type="number" class="form-control" name="udp_port"
                                       placeholder="5000" value="5000">
                            </div>
                            <div class="col-md-3 mb-3">
                                <label class="form-label">TTL</label>
                                <input type="number" class="form-control" name="udp_ttl"
                                       value="64" min="1" max="255">
                            </div>
                        </div>
                    </div>

                    <!-- SRT Fields -->
                    <div id="out-srt-fields">
                        <hr>
                        <h6><i class="bi bi-shield-lock me-2"></i>SRT Configuration</h6>
                        <div class="alert alert-info small mb-3">
                            <i class="bi bi-info-circle me-1"></i>
                            <strong>One-to-Many Mode:</strong> In listener mode, multiple clients can connect to the same port
                            and receive the stream simultaneously. Perfect for distribution to multiple destinations.
                        </div>
                        <div class="row">
                            <div class="col-md-4 mb-3">
                                <label class="form-label">Mode</label>
                                <select class="form-select" name="srt_mode" id="srtMode" onchange="updateSrtModeFields()">
                                    <option value="listener" selected>Listener (1:N - Recommended)</option>
                                    <option value="caller">Caller (1:1)</option>
                                </select>
                                <small class="text-muted">Listener allows multiple clients to pull</small>
                            </div>
                            <div class="col-md-4 mb-3">
                                <label class="form-label" id="srtAddressLabel">Listen Address</label>
                                <input type="text" class="form-control" name="srt_listen_address" id="srtAddress"
                                       value="0.0.0.0" placeholder="0.0.0.0">
                            </div>
                            <div class="col-md-4 mb-3">
                                <label class="form-label">Port</label>
                                <input type="number" class="form-control" name="srt_port"
                                       value="4900" min="1024" max="65535">
                            </div>
                        </div>
                        <div class="row">
                            <div class="col-md-4 mb-3">
                                <label class="form-label">Latency (ms)</label>
                                <input type="number" class="form-control" name="srt_latency"
                                       value="120" min="20" max="8000">
                                <small class="text-muted">Higher = more reliable, lower = less delay</small>
                            </div>
                            <div class="col-md-4 mb-3">
                                <label class="form-label">Max Clients</label>
                                <input type="number" class="form-control" name="srt_max_clients"
                                       value="10" min="1" max="100">
                                <small class="text-muted">Maximum simultaneous connections</small>
                            </div>
                            <div class="col-md-4 mb-3">
                                <label class="form-label">Stream ID (optional)</label>
                                <input type="text" class="form-control" name="srt_streamid" placeholder="">
                            </div>
                        </div>
                        <div class="row">
                            <div class="col-md-6 mb-3">
                                <label class="form-label">Encryption Passphrase (optional)</label>
                                <input type="password" class="form-control" name="srt_passphrase"
                                       placeholder="Leave empty for no encryption">
                            </div>
                            <div class="col-md-6 mb-3">
                                <label class="form-label">Key Length</label>
                                <select class="form-select" name="srt_pbkeylen">
                                    <option value="0">No Encryption</option>
                                    <option value="16">AES-128</option>
                                    <option value="24">AES-192</option>
                                    <option value="32">AES-256</option>
                                </select>
                            </div>
                        </div>
                    </div>

                    <!-- HLS Fields -->
                    <div id="out-hls-fields" style="display:none;">
                        <hr>
                        <h6><i class="bi bi-collection-play me-2"></i>HLS Configuration</h6>
                        <div class="row">
                            <div class="col-md-6 mb-3">
                                <label class="form-label">Output Directory</label>
                                <input type="text" class="form-control" name="hls_path"
                                       placeholder="/var/www/hls/stream">
                            </div>
                            <div class="col-md-3 mb-3">
                                <label class="form-label">Segment Duration</label>
                                <input type="number" class="form-control" name="hls_segment"
                                       value="4" min="1" max="30">
                            </div>
                            <div class="col-md-3 mb-3">
                                <label class="form-label">Playlist Size</label>
                                <input type="number" class="form-control" name="hls_playlist"
                                       value="5" min="1" max="20">
                            </div>
                        </div>
                    </div>
                </div>
                <div class="modal-footer">
                    <button type="button" class="btn btn-secondary" data-bs-dismiss="modal">Cancel</button>
                    <button type="submit" class="btn btn-primary">
                        <i class="bi bi-plus-lg me-1"></i>Create Output
                    </button>
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
    document.getElementById('out-hls-fields').style.display = type === 'hls' ? 'block' : 'none';
}

function updateSrtModeFields() {
    const mode = document.getElementById('srtMode').value;
    const addrLabel = document.getElementById('srtAddressLabel');
    const addrInput = document.getElementById('srtAddress');

    if (mode === 'listener') {
        addrLabel.textContent = 'Listen Address';
        addrInput.value = '0.0.0.0';
        addrInput.placeholder = '0.0.0.0';
    } else {
        addrLabel.textContent = 'Remote Address';
        addrInput.value = '';
        addrInput.placeholder = 'e.g., 192.168.1.100';
    }
}

// Auto-generate ID from name
document.getElementById('outputName').addEventListener('input', function() {
    const idField = document.getElementById('outputId');
    if (!idField.dataset.manual) {
        idField.value = this.value.toLowerCase()
            .replace(/[^a-z0-9]+/g, '-')
            .replace(/^-|-$/g, '');
    }
});

document.getElementById('outputId').addEventListener('input', function() {
    this.dataset.manual = this.value ? 'true' : '';
});

// Form submission
document.getElementById('addOutputForm').addEventListener('submit', function(e) {
    e.preventDefault();

    const formData = new FormData(this);
    formData.append('action', 'create');

    fetch('api/outputs.php', {
        method: 'POST',
        body: formData
    })
    .then(r => r.json())
    .then(data => {
        if (data.success) {
            location.reload();
        } else {
            alert(data.error || 'Failed to create output');
        }
    })
    .catch(err => {
        alert('Error: ' + err.message);
    });
});

function startService(type, id) {
    fetch(`api/${type}.php?action=start&id=${id}`, { method: 'POST' })
        .then(r => r.json())
        .then(data => {
            if (data.success) {
                location.reload();
            } else {
                alert(data.error || 'Failed to start service');
            }
        });
}

function stopService(type, id) {
    fetch(`api/${type}.php?action=stop&id=${id}`, { method: 'POST' })
        .then(r => r.json())
        .then(data => {
            if (data.success) {
                location.reload();
            } else {
                alert(data.error || 'Failed to stop service');
            }
        });
}

function deleteService(type, id) {
    if (confirm('Are you sure you want to delete this output? This action cannot be undone.')) {
        fetch(`api/${type}.php?action=delete&id=${id}`, { method: 'POST' })
            .then(r => r.json())
            .then(data => {
                if (data.success) {
                    location.reload();
                } else {
                    alert(data.error || 'Failed to delete output');
                }
            });
    }
}

function editService(type, id) {
    window.location.href = `${type}-edit.php?id=${id}`;
}

// Initialize on page load
document.addEventListener('DOMContentLoaded', function() {
    updateOutputFields();
});
</script>

<?php include __DIR__ . '/../templates/footer.php'; ?>
