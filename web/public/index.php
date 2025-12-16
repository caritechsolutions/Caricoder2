<?php
/**
 * CariTranscoder - Dashboard
 * Copyright (c) 2024 CariTech Solutions
 */

define('CARITRANS', true);
require_once __DIR__ . '/../includes/config.php';
require_once __DIR__ . '/../includes/auth.php';
require_once __DIR__ . '/../includes/functions.php';

// Require login
auth_require_login();

// Get current stats
$stats = get_system_stats();
$inputs = get_service_list('inputs');
$transcoders = get_service_list('transcoders');
$muxers = get_service_list('muxers');
$outputs = get_service_list('outputs');
$license = get_license_info();

// Page title
$page_title = 'Dashboard';
include __DIR__ . '/../templates/header.php';
?>

<div class="container-fluid py-4">
    <!-- Stats Cards Row -->
    <div class="row mb-4">
        <div class="col-xl-3 col-md-6 mb-4">
            <div class="card stats-card stats-card-inputs h-100">
                <div class="card-body">
                    <div class="d-flex justify-content-between align-items-center">
                        <div>
                            <h6 class="text-uppercase text-muted mb-1">Active Inputs</h6>
                            <h2 class="mb-0" id="stat-inputs"><?php echo count($inputs); ?></h2>
                            <small class="text-success" id="stat-inputs-running">
                                <?php echo count(array_filter($inputs, fn($i) => $i['status'] === 'running')); ?> running
                            </small>
                        </div>
                        <div class="stats-icon bg-gradient-primary">
                            <i class="bi bi-download"></i>
                        </div>
                    </div>
                </div>
                <div class="card-footer bg-transparent border-0 pt-0">
                    <a href="inputs.php" class="text-primary small">View all inputs <i class="bi bi-arrow-right"></i></a>
                </div>
            </div>
        </div>

        <div class="col-xl-3 col-md-6 mb-4">
            <div class="card stats-card stats-card-transcoders h-100">
                <div class="card-body">
                    <div class="d-flex justify-content-between align-items-center">
                        <div>
                            <h6 class="text-uppercase text-muted mb-1">Transcoders</h6>
                            <h2 class="mb-0" id="stat-transcoders"><?php echo count($transcoders); ?></h2>
                            <small class="text-success" id="stat-transcoders-running">
                                <?php echo count(array_filter($transcoders, fn($t) => $t['status'] === 'running')); ?> running
                            </small>
                        </div>
                        <div class="stats-icon bg-gradient-success">
                            <i class="bi bi-arrow-repeat"></i>
                        </div>
                    </div>
                </div>
                <div class="card-footer bg-transparent border-0 pt-0">
                    <a href="transcoders.php" class="text-success small">View all transcoders <i class="bi bi-arrow-right"></i></a>
                </div>
            </div>
        </div>

        <div class="col-xl-3 col-md-6 mb-4">
            <div class="card stats-card stats-card-muxers h-100">
                <div class="card-body">
                    <div class="d-flex justify-content-between align-items-center">
                        <div>
                            <h6 class="text-uppercase text-muted mb-1">Muxers</h6>
                            <h2 class="mb-0" id="stat-muxers"><?php echo count($muxers); ?></h2>
                            <small class="text-success" id="stat-muxers-running">
                                <?php echo count(array_filter($muxers, fn($m) => $m['status'] === 'running')); ?> running
                            </small>
                        </div>
                        <div class="stats-icon bg-gradient-warning">
                            <i class="bi bi-collection"></i>
                        </div>
                    </div>
                </div>
                <div class="card-footer bg-transparent border-0 pt-0">
                    <a href="muxers.php" class="text-warning small">View all muxers <i class="bi bi-arrow-right"></i></a>
                </div>
            </div>
        </div>

        <div class="col-xl-3 col-md-6 mb-4">
            <div class="card stats-card stats-card-outputs h-100">
                <div class="card-body">
                    <div class="d-flex justify-content-between align-items-center">
                        <div>
                            <h6 class="text-uppercase text-muted mb-1">Outputs</h6>
                            <h2 class="mb-0" id="stat-outputs"><?php echo count($outputs); ?></h2>
                            <small class="text-success" id="stat-outputs-running">
                                <?php echo count(array_filter($outputs, fn($o) => $o['status'] === 'running')); ?> running
                            </small>
                        </div>
                        <div class="stats-icon bg-gradient-info">
                            <i class="bi bi-upload"></i>
                        </div>
                    </div>
                </div>
                <div class="card-footer bg-transparent border-0 pt-0">
                    <a href="outputs.php" class="text-info small">View all outputs <i class="bi bi-arrow-right"></i></a>
                </div>
            </div>
        </div>
    </div>

    <!-- System Resources Row -->
    <div class="row mb-4">
        <div class="col-xl-4 col-md-6 mb-4">
            <div class="card h-100">
                <div class="card-header">
                    <h6 class="mb-0">CPU Usage</h6>
                </div>
                <div class="card-body">
                    <div class="progress mb-2" style="height: 25px;">
                        <div class="progress-bar bg-primary" role="progressbar"
                             style="width: <?php echo $stats['cpu_percent']; ?>%"
                             id="cpu-bar">
                            <?php echo $stats['cpu_percent']; ?>%
                        </div>
                    </div>
                    <small class="text-muted">
                        Load: <span id="cpu-load"><?php echo implode(' / ', $stats['load_avg']); ?></span>
                    </small>
                </div>
            </div>
        </div>

        <div class="col-xl-4 col-md-6 mb-4">
            <div class="card h-100">
                <div class="card-header">
                    <h6 class="mb-0">Memory Usage</h6>
                </div>
                <div class="card-body">
                    <div class="progress mb-2" style="height: 25px;">
                        <div class="progress-bar bg-success" role="progressbar"
                             style="width: <?php echo $stats['mem_percent']; ?>%"
                             id="mem-bar">
                            <?php echo $stats['mem_percent']; ?>%
                        </div>
                    </div>
                    <small class="text-muted">
                        <span id="mem-used"><?php echo format_bytes($stats['mem_used']); ?></span> /
                        <span id="mem-total"><?php echo format_bytes($stats['mem_total']); ?></span>
                    </small>
                </div>
            </div>
        </div>

        <div class="col-xl-4 col-md-6 mb-4">
            <div class="card h-100">
                <div class="card-header">
                    <h6 class="mb-0">Network Throughput</h6>
                </div>
                <div class="card-body">
                    <div class="d-flex justify-content-between mb-2">
                        <span><i class="bi bi-arrow-down text-success"></i> Download</span>
                        <span id="net-rx" class="fw-bold"><?php echo format_bitrate($stats['net_rx']); ?></span>
                    </div>
                    <div class="d-flex justify-content-between">
                        <span><i class="bi bi-arrow-up text-primary"></i> Upload</span>
                        <span id="net-tx" class="fw-bold"><?php echo format_bitrate($stats['net_tx']); ?></span>
                    </div>
                </div>
            </div>
        </div>
    </div>

    <!-- Active Pipelines -->
    <div class="row mb-4">
        <div class="col-12">
            <div class="card">
                <div class="card-header d-flex justify-content-between align-items-center">
                    <h5 class="mb-0">Active Pipelines</h5>
                    <a href="pipeline-builder.php" class="btn btn-primary btn-sm">
                        <i class="bi bi-plus-lg"></i> New Pipeline
                    </a>
                </div>
                <div class="card-body p-0">
                    <div class="table-responsive">
                        <table class="table table-hover mb-0">
                            <thead class="table-light">
                                <tr>
                                    <th>Pipeline</th>
                                    <th>Input</th>
                                    <th>Processing</th>
                                    <th>Output</th>
                                    <th>Bitrate</th>
                                    <th>Status</th>
                                    <th>Actions</th>
                                </tr>
                            </thead>
                            <tbody id="pipeline-list">
                                <?php
                                $pipelines = get_active_pipelines();
                                if (empty($pipelines)):
                                ?>
                                <tr>
                                    <td colspan="7" class="text-center py-4 text-muted">
                                        No active pipelines. <a href="pipeline-builder.php">Create one</a>
                                    </td>
                                </tr>
                                <?php else: foreach ($pipelines as $pipeline): ?>
                                <tr data-pipeline-id="<?php echo htmlspecialchars($pipeline['id']); ?>">
                                    <td>
                                        <strong><?php echo htmlspecialchars($pipeline['name']); ?></strong><br>
                                        <small class="text-muted"><?php echo htmlspecialchars($pipeline['id']); ?></small>
                                    </td>
                                    <td>
                                        <span class="badge bg-primary"><?php echo htmlspecialchars($pipeline['input_type']); ?></span><br>
                                        <small><?php echo htmlspecialchars($pipeline['input_source']); ?></small>
                                    </td>
                                    <td>
                                        <?php if ($pipeline['transcode_enabled']): ?>
                                        <span class="badge bg-success">
                                            <?php echo htmlspecialchars($pipeline['video_codec']); ?>
                                        </span>
                                        <?php else: ?>
                                        <span class="badge bg-secondary">Passthrough</span>
                                        <?php endif; ?>
                                    </td>
                                    <td>
                                        <span class="badge bg-info"><?php echo htmlspecialchars($pipeline['output_type']); ?></span><br>
                                        <small><?php echo htmlspecialchars($pipeline['output_dest']); ?></small>
                                    </td>
                                    <td>
                                        <span class="bitrate" data-pipeline="<?php echo htmlspecialchars($pipeline['id']); ?>">
                                            <?php echo format_bitrate($pipeline['bitrate']); ?>
                                        </span>
                                    </td>
                                    <td>
                                        <span class="badge status-badge status-<?php echo $pipeline['status']; ?>">
                                            <?php echo ucfirst($pipeline['status']); ?>
                                        </span>
                                    </td>
                                    <td>
                                        <div class="btn-group btn-group-sm">
                                            <?php if ($pipeline['status'] === 'running'): ?>
                                            <button class="btn btn-outline-warning" onclick="stopPipeline('<?php echo $pipeline['id']; ?>')"
                                                    title="Stop">
                                                <i class="bi bi-stop-fill"></i>
                                            </button>
                                            <?php else: ?>
                                            <button class="btn btn-outline-success" onclick="startPipeline('<?php echo $pipeline['id']; ?>')"
                                                    title="Start">
                                                <i class="bi bi-play-fill"></i>
                                            </button>
                                            <?php endif; ?>
                                            <a href="pipeline-edit.php?id=<?php echo urlencode($pipeline['id']); ?>"
                                               class="btn btn-outline-secondary" title="Edit">
                                                <i class="bi bi-gear"></i>
                                            </a>
                                            <button class="btn btn-outline-danger" onclick="deletePipeline('<?php echo $pipeline['id']; ?>')"
                                                    title="Delete">
                                                <i class="bi bi-trash"></i>
                                            </button>
                                        </div>
                                    </td>
                                </tr>
                                <?php endforeach; endif; ?>
                            </tbody>
                        </table>
                    </div>
                </div>
            </div>
        </div>
    </div>

    <!-- License & Cluster Info Row -->
    <div class="row">
        <div class="col-xl-6 mb-4">
            <div class="card h-100">
                <div class="card-header">
                    <h6 class="mb-0">License Information</h6>
                </div>
                <div class="card-body">
                    <div class="row">
                        <div class="col-6">
                            <p class="mb-1"><strong>Type:</strong></p>
                            <p class="mb-3">
                                <span class="badge bg-<?php echo $license['type'] === 'enterprise' ? 'success' : ($license['type'] === 'pro' ? 'primary' : 'secondary'); ?>">
                                    <?php echo ucfirst($license['type']); ?>
                                </span>
                            </p>
                            <p class="mb-1"><strong>Customer:</strong></p>
                            <p class="mb-3"><?php echo htmlspecialchars($license['customer']); ?></p>
                        </div>
                        <div class="col-6">
                            <p class="mb-1"><strong>Status:</strong></p>
                            <p class="mb-3">
                                <span class="badge bg-<?php echo $license['status'] === 'valid' ? 'success' : 'danger'; ?>">
                                    <?php echo ucfirst($license['status']); ?>
                                </span>
                            </p>
                            <p class="mb-1"><strong>Expires:</strong></p>
                            <p class="mb-3"><?php echo $license['expiry']; ?></p>
                        </div>
                    </div>
                    <hr>
                    <p class="mb-1"><strong>Limits:</strong></p>
                    <div class="d-flex flex-wrap gap-2">
                        <span class="badge bg-light text-dark">
                            Inputs: <?php echo $license['max_inputs'] ?: 'Unlimited'; ?>
                        </span>
                        <span class="badge bg-light text-dark">
                            Transcoders: <?php echo $license['max_transcoders'] ?: 'Unlimited'; ?>
                        </span>
                        <span class="badge bg-light text-dark">
                            Outputs: <?php echo $license['max_outputs'] ?: 'Unlimited'; ?>
                        </span>
                        <span class="badge bg-light text-dark">
                            HA Nodes: <?php echo $license['max_nodes'] ?: 'Unlimited'; ?>
                        </span>
                    </div>
                </div>
            </div>
        </div>

        <div class="col-xl-6 mb-4">
            <div class="card h-100">
                <div class="card-header d-flex justify-content-between align-items-center">
                    <h6 class="mb-0">Cluster Status</h6>
                    <span class="badge bg-<?php echo $stats['cluster_enabled'] ? 'success' : 'secondary'; ?>">
                        <?php echo $stats['cluster_enabled'] ? 'Active' : 'Disabled'; ?>
                    </span>
                </div>
                <div class="card-body">
                    <?php if ($stats['cluster_enabled']): ?>
                    <div class="table-responsive">
                        <table class="table table-sm">
                            <thead>
                                <tr>
                                    <th>Node</th>
                                    <th>Role</th>
                                    <th>Status</th>
                                    <th>Load</th>
                                </tr>
                            </thead>
                            <tbody>
                                <?php foreach ($stats['cluster_nodes'] as $node): ?>
                                <tr>
                                    <td><?php echo htmlspecialchars($node['name']); ?></td>
                                    <td>
                                        <span class="badge bg-<?php echo $node['role'] === 'primary' ? 'primary' : 'secondary'; ?>">
                                            <?php echo ucfirst($node['role']); ?>
                                        </span>
                                    </td>
                                    <td>
                                        <span class="badge bg-<?php echo $node['status'] === 'online' ? 'success' : 'danger'; ?>">
                                            <?php echo ucfirst($node['status']); ?>
                                        </span>
                                    </td>
                                    <td><?php echo $node['load']; ?>%</td>
                                </tr>
                                <?php endforeach; ?>
                            </tbody>
                        </table>
                    </div>
                    <?php else: ?>
                    <div class="text-center py-4">
                        <p class="text-muted mb-2">High availability cluster is not configured.</p>
                        <?php if (auth_has_permission('admin')): ?>
                        <a href="settings.php#cluster" class="btn btn-outline-primary btn-sm">Configure Cluster</a>
                        <?php endif; ?>
                    </div>
                    <?php endif; ?>
                </div>
            </div>
        </div>
    </div>
</div>

<script src="js/dashboard.js"></script>

<?php include __DIR__ . '/../templates/footer.php'; ?>
