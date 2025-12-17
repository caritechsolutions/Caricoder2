<?php
/**
 * CariTranscoder - Settings
 * Copyright (c) 2024 CariTech Solutions
 */

define('CARITRANS', true);
require_once __DIR__ . '/../includes/config.php';
require_once __DIR__ . '/../includes/auth.php';
require_once __DIR__ . '/../includes/functions.php';

auth_require_login();
auth_require_permission('admin');

$page_title = 'Settings';
include __DIR__ . '/../templates/header.php';
?>

<div class="container-fluid py-4">
    <h2 class="mb-4"><i class="bi bi-gear me-2"></i>System Settings</h2>

    <div class="row">
        <div class="col-md-3">
            <div class="card">
                <div class="card-body p-0">
                    <nav class="nav flex-column nav-pills">
                        <a class="nav-link active" data-bs-toggle="pill" href="#general">
                            <i class="bi bi-sliders me-2"></i>General
                        </a>
                        <a class="nav-link" data-bs-toggle="pill" href="#license">
                            <i class="bi bi-key me-2"></i>License
                        </a>
                        <a class="nav-link" data-bs-toggle="pill" href="#users">
                            <i class="bi bi-people me-2"></i>Users
                        </a>
                        <a class="nav-link" data-bs-toggle="pill" href="#cluster">
                            <i class="bi bi-diagram-3 me-2"></i>Cluster
                        </a>
                        <a class="nav-link" data-bs-toggle="pill" href="#backup">
                            <i class="bi bi-cloud-arrow-up me-2"></i>Backup
                        </a>
                        <a class="nav-link" data-bs-toggle="pill" href="#about">
                            <i class="bi bi-info-circle me-2"></i>About
                        </a>
                    </nav>
                </div>
            </div>
        </div>

        <div class="col-md-9">
            <div class="tab-content">
                <!-- General Settings -->
                <div class="tab-pane fade show active" id="general">
                    <div class="card">
                        <div class="card-header">
                            <h5 class="mb-0">General Settings</h5>
                        </div>
                        <div class="card-body">
                            <form id="generalSettingsForm">
                                <div class="mb-3">
                                    <label class="form-label">System Name</label>
                                    <input type="text" class="form-control" name="system_name" value="CariTranscoder">
                                </div>
                                <div class="row">
                                    <div class="col-md-6 mb-3">
                                        <label class="form-label">Log Level</label>
                                        <select class="form-select" name="log_level">
                                            <option value="debug">Debug</option>
                                            <option value="info" selected>Info</option>
                                            <option value="warning">Warning</option>
                                            <option value="error">Error</option>
                                        </select>
                                    </div>
                                    <div class="col-md-6 mb-3">
                                        <label class="form-label">Stats Interval (seconds)</label>
                                        <input type="number" class="form-control" name="stats_interval" value="5">
                                    </div>
                                </div>
                                <button type="submit" class="btn btn-primary">Save Settings</button>
                            </form>
                        </div>
                    </div>
                </div>

                <!-- License Settings -->
                <div class="tab-pane fade" id="license">
                    <div class="card">
                        <div class="card-header">
                            <h5 class="mb-0">License Management</h5>
                        </div>
                        <div class="card-body">
                            <div class="alert alert-info">
                                <strong>Current License:</strong> Free Edition
                            </div>
                            <form id="licenseForm" enctype="multipart/form-data">
                                <div class="mb-3">
                                    <label class="form-label">Upload License File</label>
                                    <input type="file" class="form-control" name="license_file" accept=".lic,.license">
                                </div>
                                <p class="text-muted small">
                                    Upload a license file (.lic) to activate your CariTranscoder license.
                                </p>
                                <button type="submit" class="btn btn-primary">Activate License</button>
                                <button type="button" class="btn btn-outline-secondary" onclick="requestLicense()">
                                    Request License
                                </button>
                            </form>
                        </div>
                    </div>
                </div>

                <!-- Users Settings -->
                <div class="tab-pane fade" id="users">
                    <div class="card">
                        <div class="card-header d-flex justify-content-between align-items-center">
                            <h5 class="mb-0">User Management</h5>
                            <button class="btn btn-primary btn-sm" data-bs-toggle="modal" data-bs-target="#addUserModal">
                                <i class="bi bi-plus-lg"></i> Add User
                            </button>
                        </div>
                        <div class="card-body p-0">
                            <table class="table table-hover mb-0">
                                <thead class="table-light">
                                    <tr>
                                        <th>Username</th>
                                        <th>Role</th>
                                        <th>Created</th>
                                        <th>Actions</th>
                                    </tr>
                                </thead>
                                <tbody>
                                    <tr>
                                        <td>admin</td>
                                        <td><span class="badge bg-danger">Admin</span></td>
                                        <td>System default</td>
                                        <td>
                                            <button class="btn btn-outline-secondary btn-sm">
                                                <i class="bi bi-key"></i> Change Password
                                            </button>
                                        </td>
                                    </tr>
                                </tbody>
                            </table>
                        </div>
                    </div>
                </div>

                <!-- Cluster Settings -->
                <div class="tab-pane fade" id="cluster">
                    <div class="card">
                        <div class="card-header">
                            <h5 class="mb-0">High Availability Cluster</h5>
                        </div>
                        <div class="card-body">
                            <div class="form-check form-switch mb-3">
                                <input class="form-check-input" type="checkbox" id="clusterEnabled">
                                <label class="form-check-label" for="clusterEnabled">Enable Cluster Mode</label>
                            </div>
                            <div id="clusterSettings" style="display:none;">
                                <div class="row">
                                    <div class="col-md-6 mb-3">
                                        <label class="form-label">Node Role</label>
                                        <select class="form-select" name="node_role">
                                            <option value="primary">Primary</option>
                                            <option value="secondary">Secondary</option>
                                        </select>
                                    </div>
                                    <div class="col-md-6 mb-3">
                                        <label class="form-label">Peer Address</label>
                                        <input type="text" class="form-control" name="peer_address" placeholder="192.168.1.100">
                                    </div>
                                </div>
                                <div class="row">
                                    <div class="col-md-6 mb-3">
                                        <label class="form-label">Heartbeat Interval (ms)</label>
                                        <input type="number" class="form-control" name="heartbeat_interval" value="1000">
                                    </div>
                                    <div class="col-md-6 mb-3">
                                        <label class="form-label">Failover Timeout (ms)</label>
                                        <input type="number" class="form-control" name="failover_timeout" value="5000">
                                    </div>
                                </div>
                                <button class="btn btn-primary">Save Cluster Settings</button>
                            </div>
                        </div>
                    </div>
                </div>

                <!-- Backup Settings -->
                <div class="tab-pane fade" id="backup">
                    <div class="card">
                        <div class="card-header">
                            <h5 class="mb-0">Backup & Restore</h5>
                        </div>
                        <div class="card-body">
                            <h6>Export Configuration</h6>
                            <p class="text-muted">Download all configuration files as a ZIP archive.</p>
                            <button class="btn btn-outline-primary mb-4" onclick="exportConfig()">
                                <i class="bi bi-download me-2"></i>Export Configuration
                            </button>

                            <hr>

                            <h6>Import Configuration</h6>
                            <p class="text-muted">Restore configuration from a backup file.</p>
                            <form id="importForm" enctype="multipart/form-data">
                                <div class="mb-3">
                                    <input type="file" class="form-control" name="backup_file" accept=".zip">
                                </div>
                                <button type="submit" class="btn btn-outline-warning">
                                    <i class="bi bi-upload me-2"></i>Import Configuration
                                </button>
                            </form>
                        </div>
                    </div>
                </div>

                <!-- About -->
                <div class="tab-pane fade" id="about">
                    <div class="card">
                        <div class="card-header">
                            <h5 class="mb-0">About CariTranscoder</h5>
                        </div>
                        <div class="card-body">
                            <div class="text-center mb-4">
                                <h3>CariTranscoder</h3>
                                <p class="text-muted">Professional Video Transcoding System</p>
                                <p><strong>Version:</strong> <?php echo CARITRANS_VERSION; ?></p>
                            </div>
                            <hr>
                            <div class="row">
                                <div class="col-md-6">
                                    <h6>System Information</h6>
                                    <table class="table table-sm">
                                        <tr><td>PHP Version</td><td><?php echo PHP_VERSION; ?></td></tr>
                                        <tr><td>OS</td><td><?php echo php_uname('s') . ' ' . php_uname('r'); ?></td></tr>
                                        <tr><td>Server</td><td><?php echo $_SERVER['SERVER_SOFTWARE'] ?? 'N/A'; ?></td></tr>
                                    </table>
                                </div>
                                <div class="col-md-6">
                                    <h6>Components</h6>
                                    <table class="table table-sm">
                                        <tr><td>GStreamer</td><td><span class="badge bg-success">Installed</span></td></tr>
                                        <tr><td>TSDuck</td><td><?php echo shell_exec('tsp --version 2>&1 | head -1') ? '<span class="badge bg-success">Installed</span>' : '<span class="badge bg-secondary">Not Installed</span>'; ?></td></tr>
                                    </table>
                                </div>
                            </div>
                            <hr>
                            <p class="text-center text-muted small">
                                &copy; <?php echo date('Y'); ?> CariTech Solutions. All rights reserved.
                            </p>
                        </div>
                    </div>
                </div>
            </div>
        </div>
    </div>
</div>

<script>
document.getElementById('clusterEnabled').addEventListener('change', function() {
    document.getElementById('clusterSettings').style.display = this.checked ? 'block' : 'none';
});

function exportConfig() {
    window.location.href = 'api/backup.php?action=export';
}

function requestLicense() {
    window.location.href = 'api/license.php?action=request';
}
</script>

<?php include __DIR__ . '/../templates/footer.php'; ?>
