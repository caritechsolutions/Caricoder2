<?php
/**
 * CariTranscoder - Edit Output
 * Copyright (c) 2024 CariTech Solutions
 */

define('CARITRANS', true);
require_once __DIR__ . '/../includes/config.php';
require_once __DIR__ . '/../includes/auth.php';
require_once __DIR__ . '/../includes/functions.php';

auth_require_login();

$id = $_GET['id'] ?? '';
if (empty($id)) {
    header('Location: outputs.php');
    exit;
}

// Sanitize ID
$id = preg_replace('/[^a-zA-Z0-9_-]/', '', $id);
$config_file = CONFIG_PATH . '/outputs/' . $id . '.conf';

if (!file_exists($config_file)) {
    header('Location: outputs.php?error=notfound');
    exit;
}

$config = parse_config($config_file);
$output_name = $config['output']['name'] ?? $id;
$output_type = $config['output']['type'] ?? 'udp';
$input_buffer = $config['input']['buffer_name'] ?? '';

// UDP settings
$udp_address = $config['destination']['address'] ?? '239.1.1.1';
$udp_port = $config['destination']['port'] ?? '5000';
$udp_ttl = $config['destination']['ttl'] ?? '64';
$udp_buffer_size = $config['destination']['buffer_size'] ?? '2097152';

// SRT settings
$srt_mode = $config['destination_srt']['mode'] ?? 'listener';
$srt_listen_address = $config['destination_srt']['listen_address'] ?? '0.0.0.0';
$srt_port = $config['destination_srt']['listen_port'] ?? '4900';
$srt_latency = $config['destination_srt']['latency'] ?? '120';
$srt_passphrase = $config['destination_srt']['passphrase'] ?? '';
$srt_pbkeylen = $config['destination_srt']['pbkeylen'] ?? '0';
$srt_streamid = $config['destination_srt']['streamid'] ?? '';
$srt_max_clients = $config['destination_srt']['max_clients'] ?? '10';

// HLS settings
$hls_path = $config['destination_hls']['output_dir'] ?? '';
$hls_segment = $config['destination_hls']['segment_duration'] ?? '4';
$hls_playlist = $config['destination_hls']['playlist_length'] ?? '5';

$page_title = 'Edit Output: ' . $output_name;
include __DIR__ . '/../templates/header.php';
?>

<div class="container-fluid py-4">
    <div class="d-flex justify-content-between align-items-center mb-4">
        <h2><i class="bi bi-pencil me-2"></i>Edit Output: <?php echo htmlspecialchars($output_name); ?></h2>
        <a href="outputs.php" class="btn btn-secondary">
            <i class="bi bi-arrow-left me-1"></i>Back to Outputs
        </a>
    </div>

    <form id="editOutputForm">
        <input type="hidden" name="id" value="<?php echo htmlspecialchars($id); ?>">

        <div class="card mb-4">
            <div class="card-header">
                <h5 class="mb-0">Basic Information</h5>
            </div>
            <div class="card-body">
                <div class="row">
                    <div class="col-md-4 mb-3">
                        <label class="form-label">Output Name <span class="text-danger">*</span></label>
                        <input type="text" class="form-control" name="name"
                               value="<?php echo htmlspecialchars($output_name); ?>" required>
                    </div>
                    <div class="col-md-4 mb-3">
                        <label class="form-label">Output Type <span class="text-danger">*</span></label>
                        <select class="form-select" name="type" id="outputType" onchange="updateOutputFields()">
                            <option value="udp" <?php echo $output_type === 'udp' ? 'selected' : ''; ?>>UDP Multicast</option>
                            <option value="srt" <?php echo $output_type === 'srt' ? 'selected' : ''; ?>>SRT (One-to-Many)</option>
                            <option value="hls" <?php echo $output_type === 'hls' ? 'selected' : ''; ?>>HLS</option>
                        </select>
                    </div>
                    <div class="col-md-4 mb-3">
                        <label class="form-label">Input Buffer <span class="text-danger">*</span></label>
                        <input type="text" class="form-control" name="input_buffer"
                               value="<?php echo htmlspecialchars($input_buffer); ?>" required>
                        <small class="text-muted">Ring buffer name from muxer or transcoder</small>
                    </div>
                </div>
            </div>
        </div>

        <!-- UDP Settings -->
        <div class="card mb-4" id="udp-settings" style="<?php echo $output_type !== 'udp' ? 'display:none;' : ''; ?>">
            <div class="card-header">
                <h5 class="mb-0"><i class="bi bi-broadcast me-2"></i>UDP Destination</h5>
            </div>
            <div class="card-body">
                <div class="row">
                    <div class="col-md-4 mb-3">
                        <label class="form-label">Multicast Address</label>
                        <input type="text" class="form-control" name="udp_address"
                               value="<?php echo htmlspecialchars($udp_address); ?>">
                    </div>
                    <div class="col-md-2 mb-3">
                        <label class="form-label">Port</label>
                        <input type="number" class="form-control" name="udp_port"
                               value="<?php echo htmlspecialchars($udp_port); ?>">
                    </div>
                    <div class="col-md-2 mb-3">
                        <label class="form-label">TTL</label>
                        <input type="number" class="form-control" name="udp_ttl"
                               value="<?php echo htmlspecialchars($udp_ttl); ?>" min="1" max="255">
                    </div>
                    <div class="col-md-4 mb-3">
                        <label class="form-label">Socket Buffer Size</label>
                        <select class="form-select" name="udp_buffer_size">
                            <option value="1048576" <?php echo $udp_buffer_size == '1048576' ? 'selected' : ''; ?>>1 MB</option>
                            <option value="2097152" <?php echo $udp_buffer_size == '2097152' ? 'selected' : ''; ?>>2 MB (Default)</option>
                            <option value="4194304" <?php echo $udp_buffer_size == '4194304' ? 'selected' : ''; ?>>4 MB</option>
                            <option value="8388608" <?php echo $udp_buffer_size == '8388608' ? 'selected' : ''; ?>>8 MB</option>
                        </select>
                    </div>
                </div>
            </div>
        </div>

        <!-- SRT Settings -->
        <div class="card mb-4" id="srt-settings" style="<?php echo $output_type !== 'srt' ? 'display:none;' : ''; ?>">
            <div class="card-header">
                <h5 class="mb-0"><i class="bi bi-shield-lock me-2"></i>SRT Configuration</h5>
            </div>
            <div class="card-body">
                <div class="alert alert-info small mb-3">
                    <i class="bi bi-info-circle me-1"></i>
                    <strong>One-to-Many Mode:</strong> In listener mode, multiple clients can connect to the same port
                    and receive the stream simultaneously. Perfect for distribution to multiple destinations.
                </div>
                <div class="row">
                    <div class="col-md-4 mb-3">
                        <label class="form-label">Mode</label>
                        <select class="form-select" name="srt_mode" id="srtMode" onchange="updateSrtModeFields()">
                            <option value="listener" <?php echo $srt_mode === 'listener' ? 'selected' : ''; ?>>Listener (1:N - Recommended)</option>
                            <option value="caller" <?php echo $srt_mode === 'caller' ? 'selected' : ''; ?>>Caller (1:1)</option>
                        </select>
                        <small class="text-muted">Listener allows multiple clients to pull</small>
                    </div>
                    <div class="col-md-4 mb-3">
                        <label class="form-label" id="srtAddressLabel"><?php echo $srt_mode === 'listener' ? 'Listen Address' : 'Remote Address'; ?></label>
                        <input type="text" class="form-control" name="srt_listen_address" id="srtAddress"
                               value="<?php echo htmlspecialchars($srt_listen_address); ?>">
                    </div>
                    <div class="col-md-4 mb-3">
                        <label class="form-label">Port</label>
                        <input type="number" class="form-control" name="srt_port"
                               value="<?php echo htmlspecialchars($srt_port); ?>" min="1024" max="65535">
                    </div>
                </div>
                <div class="row">
                    <div class="col-md-3 mb-3">
                        <label class="form-label">Latency (ms)</label>
                        <input type="number" class="form-control" name="srt_latency"
                               value="<?php echo htmlspecialchars($srt_latency); ?>" min="20" max="8000">
                        <small class="text-muted">Higher = more reliable</small>
                    </div>
                    <div class="col-md-3 mb-3">
                        <label class="form-label">Max Clients</label>
                        <input type="number" class="form-control" name="srt_max_clients"
                               value="<?php echo htmlspecialchars($srt_max_clients); ?>" min="1" max="100">
                        <small class="text-muted">Maximum simultaneous connections</small>
                    </div>
                    <div class="col-md-6 mb-3">
                        <label class="form-label">Stream ID (optional)</label>
                        <input type="text" class="form-control" name="srt_streamid"
                               value="<?php echo htmlspecialchars($srt_streamid); ?>">
                    </div>
                </div>
                <div class="row">
                    <div class="col-md-6 mb-3">
                        <label class="form-label">Encryption Passphrase</label>
                        <input type="password" class="form-control" name="srt_passphrase"
                               value="<?php echo htmlspecialchars($srt_passphrase); ?>"
                               placeholder="Leave empty for no encryption">
                    </div>
                    <div class="col-md-6 mb-3">
                        <label class="form-label">Key Length</label>
                        <select class="form-select" name="srt_pbkeylen">
                            <option value="0" <?php echo $srt_pbkeylen == '0' ? 'selected' : ''; ?>>No Encryption</option>
                            <option value="16" <?php echo $srt_pbkeylen == '16' ? 'selected' : ''; ?>>AES-128</option>
                            <option value="24" <?php echo $srt_pbkeylen == '24' ? 'selected' : ''; ?>>AES-192</option>
                            <option value="32" <?php echo $srt_pbkeylen == '32' ? 'selected' : ''; ?>>AES-256</option>
                        </select>
                    </div>
                </div>
            </div>
        </div>

        <!-- HLS Settings -->
        <div class="card mb-4" id="hls-settings" style="<?php echo $output_type !== 'hls' ? 'display:none;' : ''; ?>">
            <div class="card-header">
                <h5 class="mb-0"><i class="bi bi-collection-play me-2"></i>HLS Configuration</h5>
            </div>
            <div class="card-body">
                <div class="row">
                    <div class="col-md-6 mb-3">
                        <label class="form-label">Output Directory</label>
                        <input type="text" class="form-control" name="hls_path"
                               value="<?php echo htmlspecialchars($hls_path); ?>"
                               placeholder="/var/www/hls/stream">
                    </div>
                    <div class="col-md-3 mb-3">
                        <label class="form-label">Segment Duration (s)</label>
                        <input type="number" class="form-control" name="hls_segment"
                               value="<?php echo htmlspecialchars($hls_segment); ?>" min="1" max="30">
                    </div>
                    <div class="col-md-3 mb-3">
                        <label class="form-label">Playlist Size</label>
                        <input type="number" class="form-control" name="hls_playlist"
                               value="<?php echo htmlspecialchars($hls_playlist); ?>" min="1" max="20">
                    </div>
                </div>
            </div>
        </div>

        <div class="d-flex gap-2">
            <button type="submit" class="btn btn-primary">
                <i class="bi bi-check-lg me-1"></i>Save Changes
            </button>
            <a href="outputs.php" class="btn btn-secondary">Cancel</a>
            <button type="button" class="btn btn-danger ms-auto" onclick="deleteOutput()">
                <i class="bi bi-trash me-1"></i>Delete Output
            </button>
        </div>
    </form>
</div>

<script>
function updateOutputFields() {
    const type = document.getElementById('outputType').value;
    document.getElementById('udp-settings').style.display = type === 'udp' ? 'block' : 'none';
    document.getElementById('srt-settings').style.display = type === 'srt' ? 'block' : 'none';
    document.getElementById('hls-settings').style.display = type === 'hls' ? 'block' : 'none';
}

function updateSrtModeFields() {
    const mode = document.getElementById('srtMode').value;
    const addrLabel = document.getElementById('srtAddressLabel');
    const addrInput = document.getElementById('srtAddress');

    if (mode === 'listener') {
        addrLabel.textContent = 'Listen Address';
        if (!addrInput.value || addrInput.value === '') {
            addrInput.value = '0.0.0.0';
        }
    } else {
        addrLabel.textContent = 'Remote Address';
        if (addrInput.value === '0.0.0.0') {
            addrInput.value = '';
        }
    }
}

document.getElementById('editOutputForm').addEventListener('submit', function(e) {
    e.preventDefault();

    const formData = new FormData(this);
    formData.append('action', 'update');

    fetch('api/outputs.php', {
        method: 'POST',
        body: formData
    })
    .then(r => r.json())
    .then(data => {
        if (data.success) {
            // Show success message
            const btn = document.querySelector('button[type="submit"]');
            const originalText = btn.innerHTML;
            btn.innerHTML = '<i class="bi bi-check-lg me-1"></i>Saved!';
            btn.classList.remove('btn-primary');
            btn.classList.add('btn-success');
            setTimeout(() => {
                btn.innerHTML = originalText;
                btn.classList.remove('btn-success');
                btn.classList.add('btn-primary');
            }, 2000);
        } else {
            alert(data.error || 'Failed to save output');
        }
    })
    .catch(err => {
        alert('Error: ' + err.message);
    });
});

function deleteOutput() {
    if (confirm('Are you sure you want to delete this output? This action cannot be undone.')) {
        const id = document.querySelector('input[name="id"]').value;

        fetch(`api/outputs.php?action=delete&id=${id}`, { method: 'POST' })
            .then(r => r.json())
            .then(data => {
                if (data.success) {
                    window.location.href = 'outputs.php';
                } else {
                    alert(data.error || 'Failed to delete output');
                }
            })
            .catch(err => {
                alert('Error: ' + err.message);
            });
    }
}
</script>

<?php include __DIR__ . '/../templates/footer.php'; ?>
