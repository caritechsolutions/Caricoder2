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
$output_type = $config['output']['type'] ?? 'srt';

// UDP Input settings (common to both SRT and RIST)
$input_address = $config['input']['address'] ?? '';
$input_port = $config['input']['port'] ?? '5000';
$input_interface = $config['input']['interface'] ?? '';

// SRT settings
$srt_listen_address = $config['destination_srt']['listen_address'] ?? '0.0.0.0';
$srt_port = $config['destination_srt']['listen_port'] ?? '4900';
$srt_latency = $config['destination_srt']['latency'] ?? '120';
$srt_passphrase = $config['destination_srt']['passphrase'] ?? '';
$srt_pbkeylen = $config['destination_srt']['pbkeylen'] ?? '0';
$srt_streamid = $config['destination_srt']['streamid'] ?? '';
$srt_max_clients = $config['destination_srt']['max_clients'] ?? '10';

// RIST settings
$rist_mode = $config['destination_rist']['mode'] ?? 'caller';
$rist_address = $config['destination_rist']['address'] ?? '';
$rist_port = $config['destination_rist']['port'] ?? '5001';
$rist_profile = $config['destination_rist']['profile'] ?? '1';
$rist_buffer = $config['destination_rist']['buffer'] ?? '250';
$rist_encryption = $config['destination_rist']['encryption'] ?? '0';
$rist_secret = $config['destination_rist']['secret'] ?? '';
$rist_cname = $config['destination_rist']['cname'] ?? '';
$rist_npd = ($config['destination_rist']['npd'] ?? 'false') === 'true';
$rist_bandwidth = $config['destination_rist']['bandwidth'] ?? '0';
$rist_congestion = $config['destination_rist']['congestion_control'] ?? '1';
$rist_log_level = $config['destination_rist']['log_level'] ?? '6';

// HTTP settings
$http_listen_address = $config['destination_http']['listen_address'] ?? '0.0.0.0';
$http_port = $config['destination_http']['listen_port'] ?? '8888';
$http_stream_path = $config['destination_http']['stream_path'] ?? '/stream';
$http_stats_path = $config['destination_http']['stats_path'] ?? '/stats';
$http_mime_type = $config['destination_http']['mime_type'] ?? 'video/mp2t';
$http_chunked = ($config['destination_http']['chunked_encoding'] ?? 'false') === 'true';

// HLS settings (no HTTP port - files served by nginx)
$hls_output_dir = $config['destination_hls']['output_dir'] ?? '/var/www/caritrans/public/hls/' . $id;
$hls_segment_duration = $config['destination_hls']['segment_duration'] ?? '2';
$hls_segment_count = $config['destination_hls']['segment_count'] ?? '5';
$hls_variants = $config['destination_hls']['variants'] ?? '1';

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

        <!-- Basic Info -->
        <div class="card mb-4">
            <div class="card-header">
                <h5 class="mb-0">Basic Information</h5>
            </div>
            <div class="card-body">
                <div class="row">
                    <div class="col-md-6 mb-3">
                        <label class="form-label">Output Name <span class="text-danger">*</span></label>
                        <input type="text" class="form-control" name="name"
                               value="<?php echo htmlspecialchars($output_name); ?>" required>
                    </div>
                    <div class="col-md-6 mb-3">
                        <label class="form-label">Output Type</label>
                        <input type="text" class="form-control" value="<?php echo strtoupper($output_type); ?>" disabled>
                        <small class="text-muted">Type cannot be changed after creation</small>
                    </div>
                </div>
            </div>
        </div>

        <!-- UDP Input Section -->
        <div class="card mb-4">
            <div class="card-header">
                <h5 class="mb-0"><i class="bi bi-arrow-down-circle me-2"></i>UDP Input (from mux/transcoder)</h5>
            </div>
            <div class="card-body">
                <div class="row">
                    <div class="col-md-5 mb-3">
                        <label class="form-label">Input Address</label>
                        <input type="text" class="form-control" name="input_address"
                               value="<?php echo htmlspecialchars($input_address); ?>"
                               placeholder="239.1.1.1 or leave empty for unicast">
                        <small class="text-muted">Multicast group or empty for any unicast</small>
                    </div>
                    <div class="col-md-3 mb-3">
                        <label class="form-label">Input Port <span class="text-danger">*</span></label>
                        <input type="number" class="form-control" name="input_port"
                               value="<?php echo htmlspecialchars($input_port); ?>"
                               min="1024" max="65535" required>
                    </div>
                    <div class="col-md-4 mb-3">
                        <label class="form-label">Interface (optional)</label>
                        <input type="text" class="form-control" name="input_interface"
                               value="<?php echo htmlspecialchars($input_interface); ?>"
                               placeholder="e.g., 192.168.1.100">
                        <small class="text-muted">For multicast source selection</small>
                    </div>
                </div>
            </div>
        </div>

        <?php if ($output_type === 'srt'): ?>
        <!-- SRT Settings -->
        <div class="card mb-4">
            <div class="card-header">
                <h5 class="mb-0"><i class="bi bi-shield-lock me-2"></i>SRT Output (One-to-Many)</h5>
            </div>
            <div class="card-body">
                <div class="row">
                    <div class="col-md-4 mb-3">
                        <label class="form-label">Listen Address</label>
                        <input type="text" class="form-control" name="srt_listen_address"
                               value="<?php echo htmlspecialchars($srt_listen_address); ?>"
                               placeholder="0.0.0.0">
                        <small class="text-muted">0.0.0.0 = all interfaces</small>
                    </div>
                    <div class="col-md-4 mb-3">
                        <label class="form-label">SRT Port <span class="text-danger">*</span></label>
                        <input type="number" class="form-control" name="srt_port"
                               value="<?php echo htmlspecialchars($srt_port); ?>"
                               min="1024" max="65535">
                    </div>
                    <div class="col-md-4 mb-3">
                        <label class="form-label">Max Clients</label>
                        <input type="number" class="form-control" name="srt_max_clients"
                               value="<?php echo htmlspecialchars($srt_max_clients); ?>"
                               min="1" max="100">
                    </div>
                </div>
                <div class="row">
                    <div class="col-md-4 mb-3">
                        <label class="form-label">Latency (ms)</label>
                        <input type="number" class="form-control" name="srt_latency"
                               value="<?php echo htmlspecialchars($srt_latency); ?>"
                               min="20" max="8000">
                    </div>
                    <div class="col-md-4 mb-3">
                        <label class="form-label">Stream ID (optional)</label>
                        <input type="text" class="form-control" name="srt_streamid"
                               value="<?php echo htmlspecialchars($srt_streamid); ?>">
                    </div>
                    <div class="col-md-4 mb-3">
                        <label class="form-label">Key Length</label>
                        <select class="form-select" name="srt_pbkeylen">
                            <option value="0" <?php echo $srt_pbkeylen == '0' ? 'selected' : ''; ?>>No Encryption</option>
                            <option value="16" <?php echo $srt_pbkeylen == '16' ? 'selected' : ''; ?>>AES-128</option>
                            <option value="24" <?php echo $srt_pbkeylen == '24' ? 'selected' : ''; ?>>AES-192</option>
                            <option value="32" <?php echo $srt_pbkeylen == '32' ? 'selected' : ''; ?>>AES-256</option>
                        </select>
                    </div>
                </div>
                <div class="row">
                    <div class="col-md-12 mb-3">
                        <label class="form-label">Encryption Passphrase (optional)</label>
                        <input type="password" class="form-control" name="srt_passphrase"
                               value="<?php echo htmlspecialchars($srt_passphrase); ?>"
                               placeholder="Leave empty for no encryption">
                    </div>
                </div>
            </div>
        </div>
        <?php endif; ?>

        <?php if ($output_type === 'rist'): ?>
        <!-- RIST Settings -->
        <div class="card mb-4">
            <div class="card-header">
                <h5 class="mb-0"><i class="bi bi-arrow-repeat me-2"></i>RIST Output</h5>
            </div>
            <div class="card-body">
                <div class="row">
                    <div class="col-md-4 mb-3">
                        <label class="form-label">Mode</label>
                        <select class="form-select" name="rist_mode" id="ristMode">
                            <option value="caller" <?php echo $rist_mode === 'caller' ? 'selected' : ''; ?>>Caller (push to receiver)</option>
                            <option value="listener" <?php echo $rist_mode === 'listener' ? 'selected' : ''; ?>>Listener (receiver connects)</option>
                        </select>
                        <small class="text-muted">Caller pushes, Listener waits</small>
                    </div>
                    <div class="col-md-4 mb-3">
                        <label class="form-label">Destination Address <span class="text-danger">*</span></label>
                        <input type="text" class="form-control" name="rist_address"
                               value="<?php echo htmlspecialchars($rist_address); ?>"
                               placeholder="192.168.1.100">
                    </div>
                    <div class="col-md-4 mb-3">
                        <label class="form-label">RIST Port <span class="text-danger">*</span></label>
                        <input type="number" class="form-control" name="rist_port"
                               value="<?php echo htmlspecialchars($rist_port); ?>"
                               min="1024" max="65535">
                    </div>
                </div>
                <div class="row">
                    <div class="col-md-3 mb-3">
                        <label class="form-label">Profile</label>
                        <select class="form-select" name="rist_profile">
                            <option value="0" <?php echo $rist_profile == '0' ? 'selected' : ''; ?>>Simple</option>
                            <option value="1" <?php echo $rist_profile == '1' ? 'selected' : ''; ?>>Main</option>
                            <option value="2" <?php echo $rist_profile == '2' ? 'selected' : ''; ?>>Advanced</option>
                        </select>
                    </div>
                    <div class="col-md-3 mb-3">
                        <label class="form-label">Buffer (ms)</label>
                        <input type="number" class="form-control" name="rist_buffer"
                               value="<?php echo htmlspecialchars($rist_buffer); ?>"
                               min="0" max="10000">
                        <small class="text-muted">Retransmission buffer</small>
                    </div>
                    <div class="col-md-3 mb-3">
                        <label class="form-label">Encryption</label>
                        <select class="form-select" name="rist_encryption" id="ristEncryption" onchange="toggleRistSecret()">
                            <option value="0" <?php echo $rist_encryption == '0' ? 'selected' : ''; ?>>None</option>
                            <option value="128" <?php echo $rist_encryption == '128' ? 'selected' : ''; ?>>AES-128</option>
                            <option value="256" <?php echo $rist_encryption == '256' ? 'selected' : ''; ?>>AES-256</option>
                        </select>
                    </div>
                    <div class="col-md-3 mb-3">
                        <label class="form-label">Congestion Ctrl</label>
                        <select class="form-select" name="rist_congestion">
                            <option value="0" <?php echo $rist_congestion == '0' ? 'selected' : ''; ?>>Disabled</option>
                            <option value="1" <?php echo $rist_congestion == '1' ? 'selected' : ''; ?>>Normal</option>
                            <option value="2" <?php echo $rist_congestion == '2' ? 'selected' : ''; ?>>Aggressive</option>
                        </select>
                    </div>
                </div>
                <div class="row" id="ristSecretRow" style="<?php echo $rist_encryption == '0' ? 'display:none;' : ''; ?>">
                    <div class="col-md-12 mb-3">
                        <label class="form-label">Encryption Secret</label>
                        <input type="password" class="form-control" name="rist_secret"
                               value="<?php echo htmlspecialchars($rist_secret); ?>"
                               placeholder="Encryption passphrase">
                    </div>
                </div>
                <div class="row">
                    <div class="col-md-6 mb-3">
                        <label class="form-label">Stream Name (cname)</label>
                        <input type="text" class="form-control" name="rist_cname"
                               value="<?php echo htmlspecialchars($rist_cname); ?>"
                               placeholder="Optional stream identifier">
                    </div>
                    <div class="col-md-3 mb-3">
                        <label class="form-label">Bandwidth Limit (Kbps)</label>
                        <input type="number" class="form-control" name="rist_bandwidth"
                               value="<?php echo htmlspecialchars($rist_bandwidth); ?>"
                               min="0">
                        <small class="text-muted">0 = unlimited</small>
                    </div>
                    <div class="col-md-3 mb-3">
                        <label class="form-label">Options</label>
                        <div class="form-check mt-2">
                            <input class="form-check-input" type="checkbox" name="rist_npd" value="1" id="ristNpd"
                                   <?php echo $rist_npd ? 'checked' : ''; ?>>
                            <label class="form-check-label" for="ristNpd">
                                Null Packet Deletion
                            </label>
                        </div>
                    </div>
                </div>
            </div>
        </div>
        <?php endif; ?>

        <?php if ($output_type === 'http'): ?>
        <!-- HTTP Settings -->
        <div class="card mb-4">
            <div class="card-header">
                <h5 class="mb-0"><i class="bi bi-globe me-2"></i>HTTP MPEG-TS Output (Pull)</h5>
            </div>
            <div class="card-body">
                <div class="alert alert-info small mb-3">
                    <i class="bi bi-info-circle me-1"></i>
                    Clients can pull the stream via: <code>http://&lt;server&gt;:<?php echo htmlspecialchars($http_port); ?><?php echo htmlspecialchars($http_stream_path); ?></code>
                </div>
                <div class="row">
                    <div class="col-md-4 mb-3">
                        <label class="form-label">Listen Address</label>
                        <input type="text" class="form-control" name="http_listen_address"
                               value="<?php echo htmlspecialchars($http_listen_address); ?>"
                               placeholder="0.0.0.0">
                        <small class="text-muted">0.0.0.0 = all interfaces</small>
                    </div>
                    <div class="col-md-4 mb-3">
                        <label class="form-label">HTTP Port <span class="text-danger">*</span></label>
                        <input type="number" class="form-control" name="http_port"
                               value="<?php echo htmlspecialchars($http_port); ?>"
                               min="1024" max="65535">
                    </div>
                    <div class="col-md-4 mb-3">
                        <label class="form-label">Stream Path</label>
                        <input type="text" class="form-control" name="http_stream_path"
                               value="<?php echo htmlspecialchars($http_stream_path); ?>"
                               placeholder="/stream">
                        <small class="text-muted">URL path (with leading /)</small>
                    </div>
                </div>
                <div class="row">
                    <div class="col-md-4 mb-3">
                        <label class="form-label">Stats Path</label>
                        <input type="text" class="form-control" name="http_stats_path"
                               value="<?php echo htmlspecialchars($http_stats_path); ?>"
                               placeholder="/stats">
                        <small class="text-muted">JSON API endpoint</small>
                    </div>
                    <div class="col-md-4 mb-3">
                        <label class="form-label">MIME Type</label>
                        <select class="form-select" name="http_mime_type">
                            <option value="video/mp2t" <?php echo $http_mime_type === 'video/mp2t' ? 'selected' : ''; ?>>video/mp2t (MPEG-TS)</option>
                            <option value="application/octet-stream" <?php echo $http_mime_type === 'application/octet-stream' ? 'selected' : ''; ?>>application/octet-stream</option>
                        </select>
                    </div>
                    <div class="col-md-4 mb-3">
                        <label class="form-label">Options</label>
                        <div class="form-check mt-2">
                            <input class="form-check-input" type="checkbox" name="http_chunked" value="1" id="httpChunked"
                                   <?php echo $http_chunked ? 'checked' : ''; ?>>
                            <label class="form-check-label" for="httpChunked">
                                Use Chunked Transfer Encoding
                            </label>
                        </div>
                    </div>
                </div>
            </div>
        </div>
        <?php endif; ?>

        <?php if ($output_type === 'hls'): ?>
        <!-- HLS Settings -->
        <div class="card mb-4">
            <div class="card-header">
                <h5 class="mb-0"><i class="bi bi-collection-play me-2"></i>HLS Output (HTTP Live Streaming)</h5>
            </div>
            <div class="card-body">
                <div class="alert alert-info small mb-3">
                    <i class="bi bi-info-circle me-1"></i>
                    HLS files are served by nginx. Playlist: <code>/hls/<?php echo htmlspecialchars(basename($hls_output_dir)); ?>/playlist.m3u8</code>
                </div>
                <div class="row">
                    <div class="col-md-6 mb-3">
                        <label class="form-label">Output Directory</label>
                        <input type="text" class="form-control" name="hls_output_dir"
                               value="<?php echo htmlspecialchars($hls_output_dir); ?>">
                        <small class="text-muted">Directory where HLS files are written (served by nginx)</small>
                    </div>
                    <div class="col-md-6 mb-3">
                        <label class="form-label">Variants (ABR)</label>
                        <select class="form-select" name="hls_variants">
                            <option value="1" <?php echo $hls_variants == '1' ? 'selected' : ''; ?>>1 - Single stream</option>
                            <option value="2" <?php echo $hls_variants == '2' ? 'selected' : ''; ?>>2 - Two quality levels</option>
                            <option value="3" <?php echo $hls_variants == '3' ? 'selected' : ''; ?>>3 - Three quality levels</option>
                            <option value="4" <?php echo $hls_variants == '4' ? 'selected' : ''; ?>>4 - Four quality levels</option>
                        </select>
                        <small class="text-muted">Set to match ABR transcoder output count</small>
                    </div>
                </div>
                <div class="row">
                    <div class="col-md-6 mb-3">
                        <label class="form-label">Segment Duration (sec)</label>
                        <input type="number" class="form-control" name="hls_segment_duration"
                               value="<?php echo htmlspecialchars($hls_segment_duration); ?>"
                               min="1" max="10">
                    </div>
                    <div class="col-md-6 mb-3">
                        <label class="form-label">Segments to Keep</label>
                        <input type="number" class="form-control" name="hls_segment_count"
                               value="<?php echo htmlspecialchars($hls_segment_count); ?>"
                               min="2" max="20">
                    </div>
                </div>
            </div>
        </div>
        <?php endif; ?>

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
function toggleRistSecret() {
    const encryption = document.getElementById('ristEncryption').value;
    document.getElementById('ristSecretRow').style.display = encryption !== '0' ? 'block' : 'none';
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
