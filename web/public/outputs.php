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

// Helper function to get output status
function get_output_status_local($id, $type = 'srt') {
    $service_name = "{$id}-output-{$type}.service";
    exec("systemctl is-active " . escapeshellarg($service_name) . " 2>/dev/null", $output, $ret);
    if ($ret === 0 && !empty($output) && trim($output[0]) === 'active') {
        return 'running';
    }
    return 'stopped';
}

// Enhance output data with status and config details
foreach ($outputs as &$output) {
    $config_file = CONFIG_PATH . '/outputs/' . $output['id'] . '.conf';
    if (file_exists($config_file)) {
        $config = parse_config($config_file);
        $output['input'] = $config['input'] ?? [];
        $output['destination_srt'] = $config['destination_srt'] ?? [];
        $output['destination_rist'] = $config['destination_rist'] ?? [];
        $output['output'] = $config['output'] ?? [];
    }

    // Get type - first try from nested output config, then from top-level type field
    $type = $output['output']['type'] ?? ($output['type'] ?? 'srt');
    $output['resolved_type'] = $type; // Store resolved type for consistent access

    // Get running status
    $output['status'] = get_output_status_local($output['id'], $type);
}

$page_title = 'Outputs';
include __DIR__ . '/../templates/header.php';
?>

<div class="container-fluid py-4">
    <!-- DEBUG: Output data inspection -->
    <div class="alert alert-warning mb-3">
        <strong>DEBUG INFO:</strong><br>
        <strong>CONFIG_PATH:</strong> <?php echo CONFIG_PATH; ?><br>
        <strong>Config files in outputs dir:</strong> <?php
            $config_dir = CONFIG_PATH . '/outputs';
            if (is_dir($config_dir)) {
                $files = glob($config_dir . '/*.conf');
                echo count($files) . ' files: ' . implode(', ', array_map('basename', $files));
            } else {
                echo 'Directory not found: ' . $config_dir;
            }
        ?><br>
        <strong>Outputs from get_service_list:</strong> Found <?php echo count($outputs); ?> output(s)<br>
        <?php foreach ($outputs as $o): ?>
        <code>ID: <?php echo htmlspecialchars($o['id']); ?> | Name: <?php echo htmlspecialchars($o['name']); ?> | Type(top): <?php echo htmlspecialchars($o['type'] ?? 'null'); ?> | Type(nested): <?php echo htmlspecialchars($o['output']['type'] ?? 'null'); ?> | Resolved: <?php echo htmlspecialchars($o['resolved_type'] ?? 'null'); ?></code><br>
        <?php endforeach; ?>
    </div>
    <!-- END DEBUG -->

    <div class="d-flex justify-content-between align-items-center mb-4">
        <h2><i class="bi bi-upload me-2"></i>Outputs</h2>
        <button class="btn btn-primary" data-bs-toggle="modal" data-bs-target="#addOutputModal">
            <i class="bi bi-plus-lg me-1"></i>Add Output
        </button>
    </div>

    <!-- Outputs Table -->
    <div class="card">
        <div class="card-body p-0">
            <div class="table-responsive">
                <table class="table table-hover mb-0" id="outputs-table">
                    <thead class="table-dark">
                        <tr>
                            <th style="width: 40px;"></th>
                            <th>Name</th>
                            <th>Type</th>
                            <th>UDP Input</th>
                            <th>Destination</th>
                            <th>Status</th>
                            <th style="width: 220px;">Actions</th>
                        </tr>
                    </thead>
                    <tbody>
                        <?php if (empty($outputs)): ?>
                        <tr>
                            <td colspan="7" class="text-center py-5">
                                <i class="bi bi-upload text-muted" style="font-size: 3rem;"></i>
                                <h5 class="mt-3">No Outputs</h5>
                                <p class="text-muted">Create an output to send your streams via SRT or RIST.</p>
                                <button class="btn btn-primary" data-bs-toggle="modal" data-bs-target="#addOutputModal">
                                    <i class="bi bi-plus-lg me-1"></i>Add Output
                                </button>
                            </td>
                        </tr>
                        <?php else: ?>
                        <?php foreach ($outputs as $output): ?>
                        <?php
                        $type = $output['resolved_type'] ?? ($output['output']['type'] ?? ($output['type'] ?? 'srt'));
                        $input_addr = $output['input']['address'] ?? '';
                        $input_port = $output['input']['port'] ?? '';

                        // Get destination info based on type
                        if ($type === 'rist') {
                            $rist_mode = $output['destination_rist']['mode'] ?? 'caller';
                            $rist_addr = $output['destination_rist']['address'] ?? '';
                            $rist_port = $output['destination_rist']['port'] ?? '';
                            $dest_display = ($rist_mode === 'listener' ? '@' : '') . "{$rist_addr}:{$rist_port}";
                            $type_badge = 'bg-warning text-dark';
                            $type_icon = 'bi-arrow-repeat';
                        } else {
                            $srt_addr = $output['destination_srt']['listen_address'] ?? '0.0.0.0';
                            $srt_port = $output['destination_srt']['listen_port'] ?? '';
                            $dest_display = "{$srt_addr}:{$srt_port}";
                            $type_badge = 'bg-primary';
                            $type_icon = 'bi-shield-lock';
                        }
                        ?>
                        <tr data-id="<?php echo htmlspecialchars($output['id']); ?>" data-type="<?php echo $type; ?>">
                            <td>
                                <span class="status-dot status-<?php echo $output['status'] ?? 'stopped'; ?>"></span>
                            </td>
                            <td>
                                <strong><?php echo htmlspecialchars($output['name']); ?></strong>
                                <br><small class="text-muted"><?php echo htmlspecialchars($output['id']); ?></small>
                            </td>
                            <td>
                                <span class="badge <?php echo $type_badge; ?>">
                                    <i class="bi <?php echo $type_icon; ?> me-1"></i><?php echo strtoupper($type); ?>
                                </span>
                            </td>
                            <td>
                                <code><?php
                                if ($input_addr && $input_port) {
                                    echo htmlspecialchars("{$input_addr}:{$input_port}");
                                } elseif ($input_port) {
                                    echo htmlspecialchars("*:{$input_port}");
                                } else {
                                    echo '<span class="text-muted">Not set</span>';
                                }
                                ?></code>
                            </td>
                            <td>
                                <code><?php echo htmlspecialchars($dest_display); ?></code>
                                <?php if ($type === 'rist'): ?>
                                <br><small class="text-muted"><?php echo ucfirst($rist_mode); ?> mode</small>
                                <?php endif; ?>
                            </td>
                            <td>
                                <span class="badge bg-<?php echo ($output['status'] ?? 'stopped') === 'running' ? 'success' : 'secondary'; ?>">
                                    <?php echo ucfirst($output['status'] ?? 'stopped'); ?>
                                </span>
                            </td>
                            <td>
                                <div class="btn-group btn-group-sm">
                                    <?php if (($output['status'] ?? 'stopped') === 'running'): ?>
                                    <button class="btn btn-outline-warning" onclick="stopOutput('<?php echo $output['id']; ?>')" title="Stop">
                                        <i class="bi bi-stop-fill"></i>
                                    </button>
                                    <?php else: ?>
                                    <button class="btn btn-outline-success" onclick="startOutput('<?php echo $output['id']; ?>')" title="Start">
                                        <i class="bi bi-play-fill"></i>
                                    </button>
                                    <?php endif; ?>
                                    <?php if ($type === 'srt'): ?>
                                    <button class="btn btn-outline-info" onclick="showClientsModal('<?php echo $output['id']; ?>', '<?php echo htmlspecialchars($output['name']); ?>')" title="Clients">
                                        <i class="bi bi-people"></i>
                                    </button>
                                    <?php else: ?>
                                    <button class="btn btn-outline-info" onclick="showRistStatsModal('<?php echo $output['id']; ?>', '<?php echo htmlspecialchars($output['name']); ?>')" title="Stats">
                                        <i class="bi bi-graph-up"></i>
                                    </button>
                                    <?php endif; ?>
                                    <button class="btn btn-outline-secondary" onclick="editOutput('<?php echo $output['id']; ?>')" title="Edit">
                                        <i class="bi bi-pencil"></i>
                                    </button>
                                    <button class="btn btn-outline-danger" onclick="deleteOutput('<?php echo $output['id']; ?>')" title="Delete">
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
</div>

<!-- Clients Modal -->
<div class="modal fade" id="clientsModal" tabindex="-1">
    <div class="modal-dialog modal-xl">
        <div class="modal-content">
            <div class="modal-header">
                <h5 class="modal-title"><i class="bi bi-people me-2"></i>SRT Clients: <span id="clientsOutputName"></span></h5>
                <button type="button" class="btn-close" data-bs-dismiss="modal"></button>
            </div>
            <div class="modal-body">
                <input type="hidden" id="clientsOutputId">

                <!-- Summary -->
                <div class="row mb-4">
                    <div class="col-md-4">
                        <div class="card bg-light">
                            <div class="card-body text-center">
                                <h3 class="mb-0"><span id="clientCount">0</span> / <span id="maxClients">10</span></h3>
                                <small class="text-muted">Connected Clients</small>
                            </div>
                        </div>
                    </div>
                    <div class="col-md-8">
                        <div class="d-flex justify-content-end align-items-center h-100">
                            <button class="btn btn-outline-secondary" onclick="refreshClients()">
                                <i class="bi bi-arrow-clockwise"></i> Refresh
                            </button>
                        </div>
                    </div>
                </div>

                <!-- Clients List -->
                <div class="card">
                    <div class="card-header">
                        <h6 class="mb-0">Connected Clients</h6>
                    </div>
                    <div class="card-body p-0">
                        <div class="clients-list" style="max-height: 400px; overflow-y: auto;">
                            <table class="table table-hover mb-0" id="clientsTable">
                                <thead class="table-light sticky-top">
                                    <tr>
                                        <th>Slot</th>
                                        <th>Address</th>
                                        <th>Duration</th>
                                        <th>RTT</th>
                                        <th>Bandwidth</th>
                                        <th>Lost/Retrans</th>
                                        <th>Actions</th>
                                    </tr>
                                </thead>
                                <tbody id="clientsTableBody">
                                    <tr>
                                        <td colspan="7" class="text-center py-4 text-muted">
                                            <div class="spinner-border spinner-border-sm me-2"></div>
                                            Loading clients...
                                        </td>
                                    </tr>
                                </tbody>
                            </table>
                        </div>
                    </div>
                </div>

                <!-- Client Details (shown when a client is selected) -->
                <div id="clientDetails" class="mt-4 d-none">
                    <div class="card border-primary">
                        <div class="card-header bg-primary text-white d-flex justify-content-between align-items-center">
                            <h6 class="mb-0"><i class="bi bi-person-badge me-2"></i>Client Details: <span id="detailClientAddr"></span></h6>
                            <button type="button" class="btn-close btn-close-white" onclick="hideClientDetails()"></button>
                        </div>
                        <div class="card-body">
                            <div class="row">
                                <div class="col-md-3 mb-3">
                                    <small class="text-muted">Slot</small>
                                    <div class="fw-bold" id="detailSlot">-</div>
                                </div>
                                <div class="col-md-3 mb-3">
                                    <small class="text-muted">Connected Duration</small>
                                    <div class="fw-bold" id="detailDuration">-</div>
                                </div>
                                <div class="col-md-3 mb-3">
                                    <small class="text-muted">RTT</small>
                                    <div class="fw-bold" id="detailRtt">-</div>
                                </div>
                                <div class="col-md-3 mb-3">
                                    <small class="text-muted">Negotiated Latency</small>
                                    <div class="fw-bold" id="detailLatency">-</div>
                                </div>
                            </div>
                            <div class="row">
                                <div class="col-md-3 mb-3">
                                    <small class="text-muted">Send Rate</small>
                                    <div class="fw-bold" id="detailSendRate">-</div>
                                </div>
                                <div class="col-md-3 mb-3">
                                    <small class="text-muted">Est. Bandwidth</small>
                                    <div class="fw-bold" id="detailBandwidth">-</div>
                                </div>
                                <div class="col-md-3 mb-3">
                                    <small class="text-muted">Packets Sent</small>
                                    <div class="fw-bold" id="detailPacketsSent">-</div>
                                </div>
                                <div class="col-md-3 mb-3">
                                    <small class="text-muted">Send Errors</small>
                                    <div class="fw-bold" id="detailSendErrors">-</div>
                                </div>
                            </div>
                            <div class="row">
                                <div class="col-md-3 mb-3">
                                    <small class="text-muted">Packets Lost</small>
                                    <div class="fw-bold text-danger" id="detailLost">-</div>
                                </div>
                                <div class="col-md-3 mb-3">
                                    <small class="text-muted">Retransmitted</small>
                                    <div class="fw-bold text-warning" id="detailRetrans">-</div>
                                </div>
                                <div class="col-md-3 mb-3">
                                    <small class="text-muted">Dropped</small>
                                    <div class="fw-bold" id="detailDropped">-</div>
                                </div>
                                <div class="col-md-3 mb-3">
                                    <small class="text-muted">In Flight</small>
                                    <div class="fw-bold" id="detailFlight">-</div>
                                </div>
                            </div>
                            <div class="row">
                                <div class="col-md-3 mb-3">
                                    <small class="text-muted">Send Buffer</small>
                                    <div class="fw-bold" id="detailBuffer">-</div>
                                </div>
                                <div class="col-md-3 mb-3">
                                    <small class="text-muted">Congestion Window</small>
                                    <div class="fw-bold" id="detailCongestion">-</div>
                                </div>
                            </div>
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

<!-- RIST Stats Modal -->
<div class="modal fade" id="ristStatsModal" tabindex="-1">
    <div class="modal-dialog modal-xl">
        <div class="modal-content">
            <div class="modal-header">
                <h5 class="modal-title"><i class="bi bi-graph-up me-2"></i>RIST Stats: <span id="ristStatsOutputName"></span></h5>
                <button type="button" class="btn-close" data-bs-dismiss="modal"></button>
            </div>
            <div class="modal-body">
                <input type="hidden" id="ristStatsOutputId">

                <!-- Summary Stats -->
                <div class="row mb-4">
                    <div class="col-md-3">
                        <div class="card bg-light">
                            <div class="card-body text-center">
                                <h4 class="mb-0" id="ristBitrate">-</h4>
                                <small class="text-muted">Bitrate</small>
                            </div>
                        </div>
                    </div>
                    <div class="col-md-3">
                        <div class="card bg-light">
                            <div class="card-body text-center">
                                <h4 class="mb-0" id="ristRtt">-</h4>
                                <small class="text-muted">RTT</small>
                            </div>
                        </div>
                    </div>
                    <div class="col-md-3">
                        <div class="card bg-light">
                            <div class="card-body text-center">
                                <h4 class="mb-0 text-danger" id="ristLost">-</h4>
                                <small class="text-muted">Packets Lost</small>
                            </div>
                        </div>
                    </div>
                    <div class="col-md-3">
                        <div class="card bg-light">
                            <div class="card-body text-center">
                                <h4 class="mb-0 text-warning" id="ristRetrans">-</h4>
                                <small class="text-muted">Retransmitted</small>
                            </div>
                        </div>
                    </div>
                </div>

                <!-- Peer Info -->
                <div class="card mb-4">
                    <div class="card-header">
                        <h6 class="mb-0"><i class="bi bi-diagram-3 me-2"></i>Peer Statistics</h6>
                    </div>
                    <div class="card-body">
                        <div class="table-responsive">
                            <table class="table table-sm mb-0" id="ristPeersTable">
                                <thead class="table-light">
                                    <tr>
                                        <th>Peer</th>
                                        <th>State</th>
                                        <th>RTT</th>
                                        <th>Sent</th>
                                        <th>Received</th>
                                        <th>Retransmit</th>
                                        <th>Quality</th>
                                    </tr>
                                </thead>
                                <tbody id="ristPeersTableBody">
                                    <tr>
                                        <td colspan="7" class="text-center py-3 text-muted">
                                            <div class="spinner-border spinner-border-sm me-2"></div>
                                            Loading...
                                        </td>
                                    </tr>
                                </tbody>
                            </table>
                        </div>
                    </div>
                </div>

                <!-- Flow Info -->
                <div class="card">
                    <div class="card-header d-flex justify-content-between align-items-center">
                        <h6 class="mb-0"><i class="bi bi-activity me-2"></i>Flow Statistics</h6>
                        <button class="btn btn-outline-secondary btn-sm" onclick="refreshRistStats()">
                            <i class="bi bi-arrow-clockwise"></i> Refresh
                        </button>
                    </div>
                    <div class="card-body">
                        <div class="row">
                            <div class="col-md-3 mb-2">
                                <small class="text-muted">Packets Sent</small>
                                <div class="fw-bold" id="ristFlowSent">-</div>
                            </div>
                            <div class="col-md-3 mb-2">
                                <small class="text-muted">Bytes Sent</small>
                                <div class="fw-bold" id="ristFlowBytes">-</div>
                            </div>
                            <div class="col-md-3 mb-2">
                                <small class="text-muted">Recovered</small>
                                <div class="fw-bold text-success" id="ristFlowRecovered">-</div>
                            </div>
                            <div class="col-md-3 mb-2">
                                <small class="text-muted">Not Recovered</small>
                                <div class="fw-bold text-danger" id="ristFlowNotRecovered">-</div>
                            </div>
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

<!-- Add Output Modal -->
<div class="modal fade" id="addOutputModal" tabindex="-1">
    <div class="modal-dialog modal-lg">
        <div class="modal-content">
            <div class="modal-header">
                <h5 class="modal-title"><i class="bi bi-plus-lg me-2"></i>Add Output</h5>
                <button type="button" class="btn-close" data-bs-dismiss="modal"></button>
            </div>
            <form id="addOutputForm">
                <input type="hidden" name="type" id="outputType" value="srt">
                <div class="modal-body">
                    <div class="row">
                        <div class="col-md-8 mb-3">
                            <label class="form-label">Output Name <span class="text-danger">*</span></label>
                            <input type="text" class="form-control" name="name" id="outputName" required
                                   placeholder="e.g., Main Output">
                            <div id="nameValidation" class="form-text"></div>
                        </div>
                        <div class="col-md-4 mb-3">
                            <label class="form-label">Output Type <span class="text-danger">*</span></label>
                            <select class="form-select" id="outputTypeSelect" onchange="toggleOutputType()">
                                <option value="srt">SRT (One-to-Many)</option>
                                <option value="rist">RIST</option>
                            </select>
                        </div>
                    </div>

                    <!-- UDP Input Section -->
                    <hr>
                    <h6><i class="bi bi-arrow-down-circle me-2"></i>UDP Input (from mux/transcoder)</h6>
                    <div class="alert alert-info small mb-3">
                        <i class="bi bi-info-circle me-1"></i>
                        Select a source to auto-detect the UDP output, or manually configure the address and port.
                    </div>
                    <div class="row">
                        <div class="col-md-12 mb-3">
                            <label class="form-label">Source (optional)</label>
                            <select class="form-select" id="sourceSelect">
                                <option value="">-- Manual Configuration --</option>
                            </select>
                            <small class="text-muted">Select an input, transcoder, or muxer to auto-fill UDP settings</small>
                        </div>
                    </div>
                    <div class="row">
                        <div class="col-md-6 mb-3">
                            <label class="form-label">Input Address</label>
                            <input type="text" class="form-control" name="input_address" id="inputAddress"
                                   placeholder="239.1.1.1 or leave empty for unicast">
                            <small class="text-muted">Multicast group or empty for any unicast</small>
                        </div>
                        <div class="col-md-3 mb-3">
                            <label class="form-label">Input Port <span class="text-danger">*</span></label>
                            <input type="number" class="form-control" name="input_port" id="inputPort"
                                   value="5000" min="1024" max="65535" required>
                        </div>
                        <div class="col-md-3 mb-3">
                            <label class="form-label">Interface (optional)</label>
                            <input type="text" class="form-control" name="input_interface"
                                   placeholder="e.g., 192.168.1.100">
                            <small class="text-muted">For multicast</small>
                        </div>
                    </div>

                    <!-- SRT Output Section -->
                    <div id="srtSection">
                        <hr>
                        <h6><i class="bi bi-shield-lock me-2"></i>SRT Output (One-to-Many)</h6>
                        <div class="row">
                            <div class="col-md-4 mb-3">
                                <label class="form-label">Listen Address</label>
                                <input type="text" class="form-control" name="srt_listen_address"
                                       value="0.0.0.0" placeholder="0.0.0.0">
                                <small class="text-muted">0.0.0.0 = all interfaces</small>
                            </div>
                            <div class="col-md-4 mb-3">
                                <label class="form-label">SRT Port <span class="text-danger">*</span></label>
                                <input type="number" class="form-control" name="srt_port" id="srtPort"
                                       value="4900" min="1024" max="65535">
                            </div>
                            <div class="col-md-4 mb-3">
                                <label class="form-label">Max Clients</label>
                                <input type="number" class="form-control" name="srt_max_clients"
                                       value="10" min="1" max="100">
                            </div>
                        </div>
                        <div class="row">
                            <div class="col-md-4 mb-3">
                                <label class="form-label">Latency (ms)</label>
                                <input type="number" class="form-control" name="srt_latency"
                                       value="120" min="20" max="8000">
                            </div>
                            <div class="col-md-4 mb-3">
                                <label class="form-label">Stream ID (optional)</label>
                                <input type="text" class="form-control" name="srt_streamid" placeholder="">
                            </div>
                            <div class="col-md-4 mb-3">
                                <label class="form-label">Key Length</label>
                                <select class="form-select" name="srt_pbkeylen">
                                    <option value="0">No Encryption</option>
                                    <option value="16">AES-128</option>
                                    <option value="24">AES-192</option>
                                    <option value="32">AES-256</option>
                                </select>
                            </div>
                        </div>
                        <div class="row">
                            <div class="col-md-12 mb-3">
                                <label class="form-label">Encryption Passphrase (optional)</label>
                                <input type="password" class="form-control" name="srt_passphrase"
                                       placeholder="Leave empty for no encryption">
                            </div>
                        </div>
                    </div>

                    <!-- RIST Output Section -->
                    <div id="ristSection" style="display: none;">
                        <hr>
                        <h6><i class="bi bi-arrow-repeat me-2"></i>RIST Output</h6>
                        <div class="row">
                            <div class="col-md-4 mb-3">
                                <label class="form-label">Mode</label>
                                <select class="form-select" name="rist_mode" id="ristMode">
                                    <option value="caller">Caller (push to receiver)</option>
                                    <option value="listener">Listener (receiver connects)</option>
                                </select>
                                <small class="text-muted">Caller pushes, Listener waits</small>
                            </div>
                            <div class="col-md-4 mb-3">
                                <label class="form-label">Destination Address <span class="text-danger">*</span></label>
                                <input type="text" class="form-control" name="rist_address" id="ristAddress"
                                       placeholder="192.168.1.100">
                            </div>
                            <div class="col-md-4 mb-3">
                                <label class="form-label">RIST Port <span class="text-danger">*</span></label>
                                <input type="number" class="form-control" name="rist_port" id="ristPort"
                                       value="5001" min="1024" max="65535">
                            </div>
                        </div>
                        <div class="row">
                            <div class="col-md-3 mb-3">
                                <label class="form-label">Profile</label>
                                <select class="form-select" name="rist_profile">
                                    <option value="0">Simple</option>
                                    <option value="1" selected>Main</option>
                                    <option value="2">Advanced</option>
                                </select>
                            </div>
                            <div class="col-md-3 mb-3">
                                <label class="form-label">Buffer (ms)</label>
                                <input type="number" class="form-control" name="rist_buffer"
                                       value="250" min="0" max="10000">
                                <small class="text-muted">Retransmission buffer</small>
                            </div>
                            <div class="col-md-3 mb-3">
                                <label class="form-label">Encryption</label>
                                <select class="form-select" name="rist_encryption" id="ristEncryption" onchange="toggleRistSecret()">
                                    <option value="0">None</option>
                                    <option value="128">AES-128</option>
                                    <option value="256">AES-256</option>
                                </select>
                            </div>
                            <div class="col-md-3 mb-3">
                                <label class="form-label">Congestion Ctrl</label>
                                <select class="form-select" name="rist_congestion">
                                    <option value="0">Disabled</option>
                                    <option value="1" selected>Normal</option>
                                    <option value="2">Aggressive</option>
                                </select>
                            </div>
                        </div>
                        <div class="row" id="ristSecretRow" style="display: none;">
                            <div class="col-md-12 mb-3">
                                <label class="form-label">Encryption Secret</label>
                                <input type="password" class="form-control" name="rist_secret"
                                       placeholder="Encryption passphrase">
                            </div>
                        </div>
                        <div class="row">
                            <div class="col-md-6 mb-3">
                                <label class="form-label">Stream Name (cname)</label>
                                <input type="text" class="form-control" name="rist_cname"
                                       placeholder="Optional stream identifier">
                            </div>
                            <div class="col-md-3 mb-3">
                                <label class="form-label">Bandwidth Limit (Kbps)</label>
                                <input type="number" class="form-control" name="rist_bandwidth"
                                       value="0" min="0">
                                <small class="text-muted">0 = unlimited</small>
                            </div>
                            <div class="col-md-3 mb-3">
                                <label class="form-label">Options</label>
                                <div class="form-check mt-2">
                                    <input class="form-check-input" type="checkbox" name="rist_npd" value="1" id="ristNpd">
                                    <label class="form-check-label" for="ristNpd">
                                        Null Packet Deletion
                                    </label>
                                </div>
                            </div>
                        </div>
                    </div>

                    <!-- Service Name Preview -->
                    <div class="mt-3">
                        <small class="text-muted">Service Name: </small>
                        <code id="serviceNamePreview">-</code>
                    </div>
                </div>
                <div class="modal-footer">
                    <button type="button" class="btn btn-secondary" data-bs-dismiss="modal">Cancel</button>
                    <button type="submit" class="btn btn-primary" id="createBtn">
                        <i class="bi bi-plus-lg me-1"></i>Create Output
                    </button>
                </div>
            </form>
        </div>
    </div>
</div>

<script>
let nameValid = false;
let availableSources = [];
let clientsRefreshInterval = null;
let currentOutputId = null;

// Debug: Log output data from PHP
console.log('=== DEBUG: Output Data from PHP ===');
<?php foreach ($outputs as $idx => $output): ?>
console.log('Output <?php echo $idx; ?>:', {
    id: '<?php echo addslashes($output['id'] ?? ''); ?>',
    name: '<?php echo addslashes($output['name'] ?? ''); ?>',
    type_top_level: '<?php echo addslashes($output['type'] ?? 'NOT SET'); ?>',
    type_nested: '<?php echo addslashes($output['output']['type'] ?? 'NOT SET'); ?>',
    resolved_type: '<?php echo addslashes($output['resolved_type'] ?? 'NOT SET'); ?>',
    status: '<?php echo addslashes($output['status'] ?? ''); ?>',
    config_file: '<?php echo addslashes($output['config_file'] ?? ''); ?>'
});
<?php endforeach; ?>
console.log('=== END DEBUG ===');

// Format duration from seconds to HH:MM:SS
function formatDuration(seconds) {
    const h = Math.floor(seconds / 3600);
    const m = Math.floor((seconds % 3600) / 60);
    const s = seconds % 60;
    return `${h.toString().padStart(2, '0')}:${m.toString().padStart(2, '0')}:${s.toString().padStart(2, '0')}`;
}

// Show clients modal
function showClientsModal(id, name) {
    currentOutputId = id;
    document.getElementById('clientsOutputId').value = id;
    document.getElementById('clientsOutputName').textContent = name;
    hideClientDetails();

    new bootstrap.Modal(document.getElementById('clientsModal')).show();
    refreshClients();
    startClientsAutoRefresh();
}

// Refresh clients list
async function refreshClients() {
    const id = document.getElementById('clientsOutputId').value;
    if (!id) return;

    try {
        const response = await fetch(`api/outputs.php?action=clients&id=${id}`);
        const data = await response.json();

        if (data.success) {
            document.getElementById('clientCount').textContent = data.client_count;
            document.getElementById('maxClients').textContent = data.max_clients;
            renderClientsTable(data.clients || []);
        } else {
            document.getElementById('clientsTableBody').innerHTML = `
                <tr><td colspan="7" class="text-center py-4 text-danger">
                    <i class="bi bi-exclamation-triangle me-2"></i>${data.error}
                </td></tr>`;
        }
    } catch (e) {
        console.error('Failed to get clients:', e);
        document.getElementById('clientsTableBody').innerHTML = `
            <tr><td colspan="7" class="text-center py-4 text-danger">
                <i class="bi bi-exclamation-triangle me-2"></i>Failed to connect to API
            </td></tr>`;
    }
}

// Render clients table
function renderClientsTable(clients) {
    const tbody = document.getElementById('clientsTableBody');

    if (clients.length === 0) {
        tbody.innerHTML = `
            <tr><td colspan="7" class="text-center py-4 text-muted">
                <i class="bi bi-people me-2"></i>No clients connected
            </td></tr>`;
        return;
    }

    let html = '';
    clients.forEach(client => {
        const duration = formatDuration(client.duration || 0);
        const rtt = (client.rtt_ms || 0).toFixed(1);
        const bw = (client.send_rate_mbps || 0).toFixed(2);
        const lost = client.packets_lost || 0;
        const retrans = client.packets_retrans || 0;

        html += `
            <tr class="client-row" onclick="showClientDetails(${client.slot})" style="cursor: pointer;">
                <td><span class="badge bg-secondary">${client.slot}</span></td>
                <td><code>${client.address}</code></td>
                <td>${duration}</td>
                <td>${rtt} ms</td>
                <td>${bw} Mbps</td>
                <td>
                    <span class="text-danger">${lost}</span> /
                    <span class="text-warning">${retrans}</span>
                </td>
                <td>
                    <button class="btn btn-outline-danger btn-sm" onclick="event.stopPropagation(); kickClient(${client.slot}, '${client.address}')" title="Kick Client">
                        <i class="bi bi-x-circle"></i>
                    </button>
                </td>
            </tr>`;
    });

    tbody.innerHTML = html;
}

// Show client details
async function showClientDetails(slot) {
    const id = document.getElementById('clientsOutputId').value;

    try {
        const response = await fetch(`api/outputs.php?action=client_info&id=${id}&slot=${slot}`);
        const data = await response.json();

        if (data.success && data.client) {
            const c = data.client;
            document.getElementById('detailClientAddr').textContent = c.address;
            document.getElementById('detailSlot').textContent = c.slot;
            document.getElementById('detailDuration').textContent = formatDuration(c.duration || 0);
            document.getElementById('detailRtt').textContent = (c.rtt_ms || 0).toFixed(1) + ' ms';
            document.getElementById('detailLatency').textContent = (c.negotiated_latency_ms || 0) + ' ms';
            document.getElementById('detailSendRate').textContent = (c.send_rate_mbps || 0).toFixed(2) + ' Mbps';
            document.getElementById('detailBandwidth').textContent = (c.bandwidth_mbps || 0).toFixed(2) + ' Mbps';
            document.getElementById('detailPacketsSent').textContent = (c.packets_sent || 0).toLocaleString();
            document.getElementById('detailSendErrors').textContent = c.send_errors || 0;
            document.getElementById('detailLost').textContent = c.packets_lost || 0;
            document.getElementById('detailRetrans').textContent = c.packets_retrans || 0;
            document.getElementById('detailDropped').textContent = c.packets_dropped || 0;
            document.getElementById('detailFlight').textContent = c.flight_size || 0;
            document.getElementById('detailBuffer').textContent = (c.send_buffer_ms || 0) + ' ms';
            document.getElementById('detailCongestion').textContent = c.congestion_window || 0;

            document.getElementById('clientDetails').classList.remove('d-none');
        }
    } catch (e) {
        console.error('Failed to get client details:', e);
    }
}

// Hide client details
function hideClientDetails() {
    document.getElementById('clientDetails').classList.add('d-none');
}

// Kick client
async function kickClient(slot, address) {
    if (!confirm(`Are you sure you want to kick client ${address}?`)) {
        return;
    }

    const id = document.getElementById('clientsOutputId').value;

    try {
        const response = await fetch(`api/outputs.php?action=kick_client&id=${id}&slot=${slot}`, {
            method: 'POST'
        });
        const data = await response.json();

        if (data.success) {
            hideClientDetails();
            refreshClients();
        } else {
            alert(data.error || 'Failed to kick client');
        }
    } catch (e) {
        alert('Error: ' + e.message);
    }
}

// Auto-refresh clients
function startClientsAutoRefresh() {
    stopClientsAutoRefresh();
    clientsRefreshInterval = setInterval(refreshClients, 5000);
}

function stopClientsAutoRefresh() {
    if (clientsRefreshInterval) {
        clearInterval(clientsRefreshInterval);
        clientsRefreshInterval = null;
    }
}

// Stop auto-refresh when modal closes
document.getElementById('clientsModal').addEventListener('hidden.bs.modal', function() {
    stopClientsAutoRefresh();
    currentOutputId = null;
});

// Output actions
function startOutput(id) {
    fetch(`api/outputs.php?action=start&id=${id}`, { method: 'POST' })
        .then(r => r.json())
        .then(data => {
            if (data.success) {
                location.reload();
            } else {
                alert(data.error || 'Failed to start output');
            }
        });
}

function stopOutput(id) {
    fetch(`api/outputs.php?action=stop&id=${id}`, { method: 'POST' })
        .then(r => r.json())
        .then(data => {
            if (data.success) {
                location.reload();
            } else {
                alert(data.error || 'Failed to stop output');
            }
        });
}

function deleteOutput(id) {
    if (confirm('Are you sure you want to delete this output? This action cannot be undone.')) {
        fetch(`api/outputs.php?action=delete&id=${id}`, { method: 'POST' })
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

function editOutput(id) {
    window.location.href = `outputs-edit.php?id=${id}`;
}

// RIST Stats Modal
let ristStatsRefreshInterval = null;

function showRistStatsModal(id, name) {
    currentOutputId = id;
    document.getElementById('ristStatsOutputId').value = id;
    document.getElementById('ristStatsOutputName').textContent = name;

    new bootstrap.Modal(document.getElementById('ristStatsModal')).show();
    refreshRistStats();
    startRistStatsAutoRefresh();
}

async function refreshRistStats() {
    const id = document.getElementById('ristStatsOutputId').value;
    if (!id) return;

    try {
        const response = await fetch(`api/outputs.php?action=rist_metrics&id=${id}`);
        const data = await response.json();

        if (data.success) {
            renderRistStats(data.metrics || {});
        } else {
            document.getElementById('ristPeersTableBody').innerHTML = `
                <tr><td colspan="7" class="text-center py-3 text-danger">
                    <i class="bi bi-exclamation-triangle me-2"></i>${data.error}
                </td></tr>`;
        }
    } catch (e) {
        console.error('Failed to get RIST stats:', e);
        document.getElementById('ristPeersTableBody').innerHTML = `
            <tr><td colspan="7" class="text-center py-3 text-danger">
                <i class="bi bi-exclamation-triangle me-2"></i>Failed to connect to API
            </td></tr>`;
    }
}

function renderRistStats(metrics) {
    // Extract key metrics from the grouped data
    let bitrate = '-';
    let rtt = '-';
    let lost = 0;
    let retrans = 0;
    let sent = 0;
    let bytes = 0;
    let recovered = 0;
    let notRecovered = 0;

    // Parse sender metrics
    (metrics.sender || []).forEach(m => {
        if (m.name === 'rist_sender_bandwidth') bitrate = (m.value / 1000000).toFixed(2) + ' Mbps';
    });

    // Parse peer metrics
    const peers = [];
    const peerMetrics = {};
    (metrics.peer || []).forEach(m => {
        const peer = m.labels.peer || 'unknown';
        if (!peerMetrics[peer]) peerMetrics[peer] = {};
        peerMetrics[peer][m.name] = m.value;
    });

    Object.entries(peerMetrics).forEach(([peer, pm]) => {
        const peerRtt = pm.rist_peer_rtt || 0;
        const peerSent = pm.rist_peer_sent || 0;
        const peerRecv = pm.rist_peer_received || 0;
        const peerRetx = pm.rist_peer_retransmitted || 0;
        const peerQuality = pm.rist_peer_quality || 100;
        const peerState = pm.rist_peer_state || 0;

        rtt = peerRtt.toFixed(1) + ' ms';
        retrans += peerRetx;

        peers.push({
            name: peer,
            state: peerState === 1 ? 'Connected' : 'Disconnected',
            rtt: peerRtt.toFixed(1),
            sent: peerSent,
            received: peerRecv,
            retransmit: peerRetx,
            quality: peerQuality.toFixed(1)
        });
    });

    // Parse flow metrics
    (metrics.flow || []).forEach(m => {
        if (m.name === 'rist_flow_sent') sent = m.value;
        if (m.name === 'rist_flow_sent_bytes') bytes = m.value;
        if (m.name === 'rist_flow_recovered') recovered = m.value;
        if (m.name === 'rist_flow_not_recovered') { notRecovered = m.value; lost = m.value; }
    });

    // Update summary cards
    document.getElementById('ristBitrate').textContent = bitrate;
    document.getElementById('ristRtt').textContent = rtt;
    document.getElementById('ristLost').textContent = lost.toLocaleString();
    document.getElementById('ristRetrans').textContent = retrans.toLocaleString();

    // Update flow stats
    document.getElementById('ristFlowSent').textContent = sent.toLocaleString();
    document.getElementById('ristFlowBytes').textContent = formatBytes(bytes);
    document.getElementById('ristFlowRecovered').textContent = recovered.toLocaleString();
    document.getElementById('ristFlowNotRecovered').textContent = notRecovered.toLocaleString();

    // Render peers table
    const tbody = document.getElementById('ristPeersTableBody');
    if (peers.length === 0) {
        tbody.innerHTML = `
            <tr><td colspan="7" class="text-center py-3 text-muted">
                <i class="bi bi-diagram-3 me-2"></i>No peer connections
            </td></tr>`;
    } else {
        let html = '';
        peers.forEach(p => {
            const stateClass = p.state === 'Connected' ? 'text-success' : 'text-danger';
            html += `
                <tr>
                    <td><code>${p.name}</code></td>
                    <td><span class="${stateClass}">${p.state}</span></td>
                    <td>${p.rtt} ms</td>
                    <td>${p.sent.toLocaleString()}</td>
                    <td>${p.received.toLocaleString()}</td>
                    <td>${p.retransmit.toLocaleString()}</td>
                    <td>${p.quality}%</td>
                </tr>`;
        });
        tbody.innerHTML = html;
    }
}

function formatBytes(bytes) {
    if (bytes === 0) return '0 B';
    const k = 1024;
    const sizes = ['B', 'KB', 'MB', 'GB', 'TB'];
    const i = Math.floor(Math.log(bytes) / Math.log(k));
    return parseFloat((bytes / Math.pow(k, i)).toFixed(2)) + ' ' + sizes[i];
}

function startRistStatsAutoRefresh() {
    stopRistStatsAutoRefresh();
    ristStatsRefreshInterval = setInterval(refreshRistStats, 5000);
}

function stopRistStatsAutoRefresh() {
    if (ristStatsRefreshInterval) {
        clearInterval(ristStatsRefreshInterval);
        ristStatsRefreshInterval = null;
    }
}

document.getElementById('ristStatsModal').addEventListener('hidden.bs.modal', function() {
    stopRistStatsAutoRefresh();
});

// Output Type Toggle
function toggleOutputType() {
    const type = document.getElementById('outputTypeSelect').value;
    document.getElementById('outputType').value = type;

    if (type === 'rist') {
        document.getElementById('srtSection').style.display = 'none';
        document.getElementById('ristSection').style.display = 'block';
    } else {
        document.getElementById('srtSection').style.display = 'block';
        document.getElementById('ristSection').style.display = 'none';
    }

    updateServiceNamePreview();
    validateName();
}

function toggleRistSecret() {
    const encryption = document.getElementById('ristEncryption').value;
    document.getElementById('ristSecretRow').style.display = encryption !== '0' ? 'block' : 'none';
}

// Add Output Modal functions
function updateServiceNamePreview() {
    const name = document.getElementById('outputName').value;
    const type = document.getElementById('outputTypeSelect').value;
    const id = name.toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '');
    const preview = document.getElementById('serviceNamePreview');
    preview.textContent = id ? id + '-output-' + type : '-';
}

function loadSources() {
    fetch('api/outputs.php?action=available_sources')
        .then(r => r.json())
        .then(data => {
            availableSources = data.sources || [];
            populateSourceDropdown();
        })
        .catch(err => console.error('Failed to load sources:', err));
}

function populateSourceDropdown() {
    const select = document.getElementById('sourceSelect');
    select.innerHTML = '<option value="">-- Manual Configuration --</option>';

    const groups = {
        input: { label: 'Inputs', items: [] },
        transcoder: { label: 'Transcoders', items: [] },
        muxer: { label: 'Muxers', items: [] }
    };

    availableSources.forEach(s => {
        if (groups[s.source_type]) {
            groups[s.source_type].items.push(s);
        }
    });

    Object.values(groups).forEach(group => {
        if (group.items.length > 0) {
            const optgroup = document.createElement('optgroup');
            optgroup.label = group.label;
            group.items.forEach(source => {
                const opt = document.createElement('option');
                opt.value = JSON.stringify(source);
                let label = source.display_name;
                if (source.output_port) {
                    label += ` (${source.output_address || '*'}:${source.output_port})`;
                }
                if (source.status === 'running') label += ' [Running]';
                opt.textContent = label;
                optgroup.appendChild(opt);
            });
            select.appendChild(optgroup);
        }
    });
}

document.getElementById('sourceSelect').addEventListener('change', function() {
    if (!this.value) return;
    try {
        const source = JSON.parse(this.value);
        document.getElementById('inputAddress').value = source.output_address || '';
        document.getElementById('inputPort').value = source.output_port || '5000';
    } catch (e) {}
});

let validateTimeout = null;
function validateName() {
    const nameInput = document.getElementById('outputName');
    const name = nameInput.value.trim();
    const validation = document.getElementById('nameValidation');
    const createBtn = document.getElementById('createBtn');
    const type = document.getElementById('outputTypeSelect').value;

    if (!name) {
        validation.textContent = '';
        validation.className = 'form-text';
        nameValid = false;
        createBtn.disabled = false;
        return;
    }

    clearTimeout(validateTimeout);
    validateTimeout = setTimeout(() => {
        fetch(`api/outputs.php?action=check_name&name=${encodeURIComponent(name)}&type=${type}`)
            .then(r => r.json())
            .then(data => {
                if (data.available) {
                    validation.textContent = 'Name available';
                    validation.className = 'form-text text-success';
                    nameInput.classList.remove('is-invalid');
                    nameInput.classList.add('is-valid');
                    nameValid = true;
                    createBtn.disabled = false;
                } else {
                    validation.textContent = data.config_exists ? 'Output already exists' : 'Service file exists';
                    validation.className = 'form-text text-danger';
                    nameInput.classList.remove('is-valid');
                    nameInput.classList.add('is-invalid');
                    nameValid = false;
                    createBtn.disabled = true;
                }
                updateServiceNamePreview();
            })
            .catch(() => {
                validation.textContent = 'Error checking name';
                validation.className = 'form-text text-warning';
            });
    }, 300);
}

document.getElementById('outputName').addEventListener('input', validateName);

document.getElementById('addOutputForm').addEventListener('submit', function(e) {
    e.preventDefault();

    if (!nameValid && document.getElementById('outputName').value.trim()) {
        alert('Please choose a unique name for the output');
        return;
    }

    const formData = new FormData(this);
    formData.append('action', 'create');

    fetch('api/outputs.php', { method: 'POST', body: formData })
        .then(r => r.json())
        .then(data => {
            if (data.success) {
                location.reload();
            } else {
                alert(data.error || 'Failed to create output');
            }
        })
        .catch(err => alert('Error: ' + err.message));
});

document.getElementById('addOutputModal').addEventListener('show.bs.modal', function() {
    document.getElementById('addOutputForm').reset();
    document.getElementById('nameValidation').textContent = '';
    document.getElementById('outputName').classList.remove('is-valid', 'is-invalid');
    document.getElementById('createBtn').disabled = false;
    document.getElementById('sourceSelect').value = '';
    document.getElementById('outputTypeSelect').value = 'srt';
    document.getElementById('outputType').value = 'srt';
    document.getElementById('srtSection').style.display = 'block';
    document.getElementById('ristSection').style.display = 'none';
    document.getElementById('ristSecretRow').style.display = 'none';
    nameValid = false;
    updateServiceNamePreview();
    loadSources();
});
</script>

<?php include __DIR__ . '/../templates/footer.php'; ?>
