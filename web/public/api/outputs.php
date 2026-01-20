<?php
/**
 * CariTranscoder - Outputs API
 * Copyright (c) 2024 CariTech Solutions
 */

// Start output buffering to catch any unexpected output
ob_start();

// Set JSON content type early
header('Content-Type: application/json');

// Error handler to convert PHP errors to JSON
set_error_handler(function($errno, $errstr, $errfile, $errline) {
    if (!(error_reporting() & $errno)) {
        return false;
    }
    ob_end_clean();
    http_response_code(500);
    echo json_encode(['error' => "PHP Error: $errstr", 'file' => basename($errfile), 'line' => $errline]);
    exit;
});

define('CARITRANS', true);
require_once __DIR__ . '/../../includes/config.php';
require_once __DIR__ . '/../../includes/auth.php';
require_once __DIR__ . '/../../includes/functions.php';

// Clear any output from includes
ob_end_clean();

// Require login
if (!auth_is_logged_in()) {
    json_response(['error' => 'Unauthorized'], 401);
}

$action = $_GET['action'] ?? $_POST['action'] ?? 'list';

switch ($action) {
    case 'list':
        $outputs = get_service_list('outputs');
        json_response(['outputs' => $outputs]);
        break;

    case 'get':
        $id = $_GET['id'] ?? '';
        if (empty($id)) {
            json_response(['error' => 'Output ID required'], 400);
        }
        $output = get_output_config($id);
        if ($output) {
            json_response($output);
        } else {
            json_response(['error' => 'Output not found'], 404);
        }
        break;

    case 'available_buffers':
        // Get all available input buffers from inputs, transcoders, and muxers
        $buffers = get_available_buffers();
        json_response(['buffers' => $buffers]);
        break;

    case 'check_name':
        $name = $_GET['name'] ?? '';
        $type = $_GET['type'] ?? 'srt';
        $exclude_id = $_GET['exclude'] ?? '';

        if (empty($name)) {
            json_response(['error' => 'Name required'], 400);
        }

        $id = sanitize_name_to_id($name);
        $service_name = $id . '-output-' . $type;

        // Check if output config exists
        $config_exists = output_exists($id, $exclude_id);

        // Check if service file exists
        $service_exists = file_exists('/etc/systemd/system/' . $service_name . '.service');

        $available = !$config_exists && !$service_exists;

        json_response([
            'available' => $available,
            'suggested_id' => $id,
            'service_name' => $service_name,
            'config_exists' => $config_exists,
            'service_exists' => $service_exists
        ]);
        break;

    case 'create':
        $data = $_POST;
        if (empty($data) || empty($data['name'])) {
            $data = json_decode(file_get_contents('php://input'), true);
        }

        if (empty($data['name'])) {
            json_response(['error' => 'Output name required'], 400);
        }

        $result = create_output($data);
        json_response($result);
        break;

    case 'update':
        $id = $_GET['id'] ?? $_POST['id'] ?? '';
        if (empty($id)) {
            json_response(['error' => 'Output ID required'], 400);
        }

        $data = $_POST;
        if (empty($data) || count($data) <= 1) {
            $data = json_decode(file_get_contents('php://input'), true);
        }

        $result = update_output($id, $data);
        json_response($result);
        break;

    case 'delete':
        $id = $_GET['id'] ?? $_POST['id'] ?? '';
        if (empty($id)) {
            json_response(['error' => 'Output ID required'], 400);
        }

        $result = delete_output($id);
        json_response($result);
        break;

    case 'start':
        $id = $_GET['id'] ?? $_POST['id'] ?? '';
        if (empty($id)) {
            json_response(['error' => 'Output ID required'], 400);
        }

        $result = start_output_service($id);
        json_response($result);
        break;

    case 'stop':
        $id = $_GET['id'] ?? $_POST['id'] ?? '';
        if (empty($id)) {
            json_response(['error' => 'Output ID required'], 400);
        }

        $result = stop_output_service($id);
        json_response($result);
        break;

    case 'status':
        $id = $_GET['id'] ?? '';
        if (empty($id)) {
            json_response(['error' => 'Output ID required'], 400);
        }

        $result = get_output_status($id);
        json_response($result);
        break;

    case 'metrics':
        $id = $_GET['id'] ?? '';
        if (empty($id)) {
            json_response(['error' => 'Output ID required'], 400);
        }

        $metrics = get_output_metrics($id);
        json_response($metrics);
        break;

    default:
        json_response(['error' => 'Invalid action'], 400);
}

/**
 * Get all available input buffers from inputs, transcoders, and muxers
 */
function get_available_buffers() {
    $buffers = [];

    // Get buffers from inputs
    $inputs = get_service_list('inputs');
    foreach ($inputs as $input) {
        $id = $input['id'] ?? '';
        $name = $input['name'] ?? $id;
        if ($id) {
            $buffers[] = [
                'buffer_name' => $id . '-out',
                'display_name' => $name . ' (Input)',
                'source_type' => 'input',
                'source_id' => $id,
                'status' => $input['status'] ?? 'unknown'
            ];
        }
    }

    // Get buffers from transcoders
    $transcoders = get_service_list('transcoders');
    foreach ($transcoders as $transcoder) {
        $id = $transcoder['id'] ?? '';
        $name = $transcoder['name'] ?? $id;
        if ($id) {
            $buffers[] = [
                'buffer_name' => $id . '-out',
                'display_name' => $name . ' (Transcoder)',
                'source_type' => 'transcoder',
                'source_id' => $id,
                'status' => $transcoder['status'] ?? 'unknown'
            ];
        }
    }

    // Get buffers from muxers
    $muxers = get_service_list('muxers');
    foreach ($muxers as $muxer) {
        $id = $muxer['id'] ?? '';
        $name = $muxer['name'] ?? $id;
        // Check for output buffer_name in config
        $buffer_name = $muxer['output']['buffer_name'] ?? ($id . '-out');
        if ($id) {
            $buffers[] = [
                'buffer_name' => $buffer_name,
                'display_name' => $name . ' (Muxer)',
                'source_type' => 'muxer',
                'source_id' => $id,
                'status' => $muxer['status'] ?? 'unknown'
            ];
        }
    }

    return $buffers;
}

/**
 * Check if output exists
 */
function output_exists($id, $exclude_id = '') {
    $id = preg_replace('/[^a-zA-Z0-9_-]/', '', $id);
    if ($id === $exclude_id) {
        return false;
    }
    $config_file = CONFIG_PATH . '/outputs/' . $id . '.conf';
    return file_exists($config_file);
}

/**
 * Get output configuration
 */
function get_output_config($id) {
    $id = preg_replace('/[^a-zA-Z0-9_-]/', '', $id);
    $config_file = CONFIG_PATH . '/outputs/' . $id . '.conf';

    if (!file_exists($config_file)) {
        return null;
    }

    $config = parse_config($config_file);
    $config['id'] = $id;

    return $config;
}

/**
 * Generate systemd service file for output
 */
function generate_output_service_file($id, $type, $name) {
    $service_name = $id . '-output-' . $type;
    $service_path = '/etc/systemd/system/' . $service_name . '.service';

    $service_content = <<<EOT
[Unit]
Description=CariTranscoder Output - {$name}
Documentation=https://github.com/caritechsolutions/caritranscoder
After=network.target
Wants=network-online.target

[Service]
Type=simple
User=caritrans
Group=caritrans

# Configuration
Environment="CONFIG_DIR=/etc/caritrans"
Environment="RUN_DIR=/run/caritrans"
Environment="LOG_DIR=/var/log/caritrans"
Environment="GST_PLUGIN_PATH=/usr/lib/gstreamer-1.0"

# Main process
ExecStart=/usr/local/bin/cari-output --config \${CONFIG_DIR}/outputs/{$id}.conf
ExecReload=/bin/kill -HUP \$MAINPID

# Restart behavior
Restart=always
RestartSec=5
StartLimitIntervalSec=60
StartLimitBurst=5

# Watchdog
WatchdogSec=30

# Resource limits
LimitNOFILE=65535
LimitNPROC=4096

# Security
NoNewPrivileges=true
ProtectSystem=strict
ProtectHome=true
PrivateTmp=true
ReadWritePaths=/run/caritrans /var/log/caritrans /dev/shm

# Network capabilities for multicast
AmbientCapabilities=CAP_NET_RAW CAP_NET_ADMIN

# Logging
StandardOutput=journal
StandardError=journal
SyslogIdentifier={$service_name}

[Install]
WantedBy=multi-user.target
EOT;

    // Write service file
    $cmd = "echo " . escapeshellarg($service_content) . " | sudo tee " . escapeshellarg($service_path) . " > /dev/null 2>&1";
    shell_exec($cmd);

    // Reload systemd
    shell_exec("sudo /bin/systemctl daemon-reload 2>&1");

    return $service_name;
}

/**
 * Delete systemd service file for output
 */
function delete_output_service_file($id, $type) {
    $service_name = $id . '-output-' . $type;
    $service_path = '/etc/systemd/system/' . $service_name . '.service';

    if (file_exists($service_path)) {
        // Stop and disable service first
        shell_exec("sudo /bin/systemctl stop " . escapeshellarg($service_name) . " 2>&1");
        shell_exec("sudo /bin/systemctl disable " . escapeshellarg($service_name) . " 2>&1");

        // Remove service file
        shell_exec("sudo rm -f " . escapeshellarg($service_path) . " 2>&1");

        // Reload systemd
        shell_exec("sudo /bin/systemctl daemon-reload 2>&1");
    }

    return true;
}

/**
 * Get service name for output
 */
function get_output_service_name($id) {
    $config = get_output_config($id);
    if (!$config) {
        return null;
    }
    $type = $config['output']['type'] ?? 'udp';
    return $id . '-output-' . $type;
}

/**
 * Create new output
 */
function create_output($data) {
    $name = trim($data['name'] ?? '');
    $id = !empty($data['id']) ? sanitize_name_to_id($data['id']) : sanitize_name_to_id($name);
    $type = $data['type'] ?? 'srt';

    if (empty($name)) {
        return ['success' => false, 'error' => 'Output name required'];
    }

    if (output_exists($id)) {
        return ['success' => false, 'error' => 'Output with this ID already exists'];
    }

    // Check for service file conflict
    $service_name = $id . '-output-' . $type;
    if (file_exists('/etc/systemd/system/' . $service_name . '.service')) {
        return ['success' => false, 'error' => 'Service file already exists: ' . $service_name];
    }

    // Build configuration
    $config = [
        'output' => [
            'id' => $id,
            'name' => $name,
            'type' => $type,
            'enabled' => 'true',
            'service_name' => $service_name
        ],
        'input' => [
            'buffer_name' => $data['input_buffer'] ?? ''
        ]
    ];

    // Type-specific configuration
    switch ($type) {
        case 'udp':
            $config['destination'] = [
                'address' => $data['udp_address'] ?? '239.1.1.1',
                'port' => $data['udp_port'] ?? '5000',
                'ttl' => $data['udp_ttl'] ?? '64',
                'buffer_size' => $data['udp_buffer_size'] ?? '2097152'
            ];
            break;

        case 'srt':
            $config['destination_srt'] = [
                'mode' => $data['srt_mode'] ?? 'listener',
                'listen_address' => $data['srt_listen_address'] ?? '0.0.0.0',
                'listen_port' => $data['srt_port'] ?? '4900',
                'latency' => $data['srt_latency'] ?? '120',
                'passphrase' => $data['srt_passphrase'] ?? '',
                'pbkeylen' => $data['srt_pbkeylen'] ?? '0',
                'streamid' => $data['srt_streamid'] ?? '',
                'max_clients' => $data['srt_max_clients'] ?? '10'
            ];
            break;

        case 'hls':
            $config['destination_hls'] = [
                'output_dir' => $data['hls_path'] ?? '/var/www/hls',
                'segment_duration' => $data['hls_segment'] ?? '4',
                'playlist_length' => $data['hls_playlist'] ?? '5'
            ];
            break;
    }

    // Save configuration
    $config_file = CONFIG_PATH . '/outputs/' . $id . '.conf';
    if (!save_config($config_file, $config)) {
        return ['success' => false, 'error' => 'Failed to save configuration'];
    }

    // Generate systemd service file
    generate_output_service_file($id, $type, $name);

    return ['success' => true, 'id' => $id, 'service_name' => $service_name, 'message' => 'Output created successfully'];
}

/**
 * Update existing output
 */
function update_output($id, $data) {
    $id = preg_replace('/[^a-zA-Z0-9_-]/', '', $id);
    $config_file = CONFIG_PATH . '/outputs/' . $id . '.conf';

    if (!file_exists($config_file)) {
        return ['success' => false, 'error' => 'Output not found'];
    }

    // Load existing config
    $config = parse_config($config_file);
    $old_type = $config['output']['type'] ?? 'udp';
    $new_type = $data['type'] ?? $old_type;

    // Stop service if running
    stop_output_service($id);

    // If type changed, delete old service file and create new one
    if ($old_type !== $new_type) {
        delete_output_service_file($id, $old_type);
    }

    // Update basic fields
    if (!empty($data['name'])) {
        $config['output']['name'] = $data['name'];
    }
    if (!empty($data['type'])) {
        $config['output']['type'] = $data['type'];
    }
    if (!empty($data['input_buffer'])) {
        $config['input']['buffer_name'] = $data['input_buffer'];
    }

    // Update service name
    $config['output']['service_name'] = $id . '-output-' . $new_type;

    // Update type-specific fields
    switch ($new_type) {
        case 'udp':
            if (!isset($config['destination'])) $config['destination'] = [];
            if (!empty($data['udp_address'])) $config['destination']['address'] = $data['udp_address'];
            if (!empty($data['udp_port'])) $config['destination']['port'] = $data['udp_port'];
            if (!empty($data['udp_ttl'])) $config['destination']['ttl'] = $data['udp_ttl'];
            break;

        case 'srt':
            if (!isset($config['destination_srt'])) $config['destination_srt'] = [];
            if (!empty($data['srt_mode'])) $config['destination_srt']['mode'] = $data['srt_mode'];
            if (!empty($data['srt_listen_address'])) $config['destination_srt']['listen_address'] = $data['srt_listen_address'];
            if (!empty($data['srt_port'])) $config['destination_srt']['listen_port'] = $data['srt_port'];
            if (!empty($data['srt_latency'])) $config['destination_srt']['latency'] = $data['srt_latency'];
            if (isset($data['srt_passphrase'])) $config['destination_srt']['passphrase'] = $data['srt_passphrase'];
            if (isset($data['srt_pbkeylen'])) $config['destination_srt']['pbkeylen'] = $data['srt_pbkeylen'];
            if (isset($data['srt_streamid'])) $config['destination_srt']['streamid'] = $data['srt_streamid'];
            if (!empty($data['srt_max_clients'])) $config['destination_srt']['max_clients'] = $data['srt_max_clients'];
            break;

        case 'hls':
            if (!isset($config['destination_hls'])) $config['destination_hls'] = [];
            if (!empty($data['hls_path'])) $config['destination_hls']['output_dir'] = $data['hls_path'];
            if (!empty($data['hls_segment'])) $config['destination_hls']['segment_duration'] = $data['hls_segment'];
            if (!empty($data['hls_playlist'])) $config['destination_hls']['playlist_length'] = $data['hls_playlist'];
            break;
    }

    if (!save_config($config_file, $config)) {
        return ['success' => false, 'error' => 'Failed to save configuration'];
    }

    // Generate/update systemd service file
    $name = $config['output']['name'] ?? $id;
    generate_output_service_file($id, $new_type, $name);

    return ['success' => true, 'message' => 'Output updated successfully'];
}

/**
 * Delete output
 */
function delete_output($id) {
    $id = preg_replace('/[^a-zA-Z0-9_-]/', '', $id);
    $config_file = CONFIG_PATH . '/outputs/' . $id . '.conf';

    if (!file_exists($config_file)) {
        return ['success' => false, 'error' => 'Output not found'];
    }

    // Get type for service file deletion
    $config = parse_config($config_file);
    $type = $config['output']['type'] ?? 'udp';

    // Stop service first
    stop_output_service($id);

    // Delete service file
    delete_output_service_file($id, $type);

    // Delete config file
    if (!unlink($config_file)) {
        return ['success' => false, 'error' => 'Failed to delete configuration'];
    }

    return ['success' => true, 'message' => 'Output deleted successfully'];
}

/**
 * Start output service
 */
function start_output_service($id) {
    $id = preg_replace('/[^a-zA-Z0-9_-]/', '', $id);
    $config_file = CONFIG_PATH . '/outputs/' . $id . '.conf';

    if (!file_exists($config_file)) {
        return ['success' => false, 'error' => 'Output not found'];
    }

    $service_name = get_output_service_name($id);
    if (!$service_name) {
        return ['success' => false, 'error' => 'Cannot determine service name'];
    }

    $cmd = "sudo /bin/systemctl start " . escapeshellarg($service_name) . " 2>&1";
    $output = shell_exec($cmd);

    // Check if started
    usleep(500000);
    $status_cmd = "systemctl is-active " . escapeshellarg($service_name) . " 2>&1";
    $status = trim(shell_exec($status_cmd));

    if ($status === 'active') {
        return ['success' => true, 'message' => 'Output started'];
    } else {
        return ['success' => false, 'error' => 'Failed to start output: ' . ($output ?: 'Unknown error')];
    }
}

/**
 * Stop output service
 */
function stop_output_service($id) {
    $id = preg_replace('/[^a-zA-Z0-9_-]/', '', $id);

    $service_name = get_output_service_name($id);
    if (!$service_name) {
        return ['success' => true, 'message' => 'No service to stop'];
    }

    $cmd = "sudo /bin/systemctl stop " . escapeshellarg($service_name) . " 2>&1";
    shell_exec($cmd);

    usleep(500000);
    $status_cmd = "systemctl is-active " . escapeshellarg($service_name) . " 2>&1";
    $status = trim(shell_exec($status_cmd));

    if ($status !== 'active') {
        return ['success' => true, 'message' => 'Output stopped'];
    } else {
        return ['success' => false, 'error' => 'Failed to stop output'];
    }
}

/**
 * Get output status
 */
function get_output_status($id) {
    $id = preg_replace('/[^a-zA-Z0-9_-]/', '', $id);
    $config_file = CONFIG_PATH . '/outputs/' . $id . '.conf';

    if (!file_exists($config_file)) {
        return ['success' => false, 'error' => 'Output not found'];
    }

    $config = parse_config($config_file);
    $service_name = get_output_service_name($id);

    $status_cmd = "systemctl is-active " . escapeshellarg($service_name) . " 2>&1";
    $status = trim(shell_exec($status_cmd));

    return [
        'success' => true,
        'id' => $id,
        'name' => $config['output']['name'] ?? $id,
        'type' => $config['output']['type'] ?? 'udp',
        'service_name' => $service_name,
        'status' => $status === 'active' ? 'running' : 'stopped'
    ];
}

/**
 * Get output metrics
 */
function get_output_metrics($id) {
    $id = preg_replace('/[^a-zA-Z0-9_-]/', '', $id);
    $config_file = CONFIG_PATH . '/outputs/' . $id . '.conf';

    if (!file_exists($config_file)) {
        return ['success' => false, 'error' => 'Output not found'];
    }

    $config = parse_config($config_file);
    $type = $config['output']['type'] ?? 'udp';
    $service_name = get_output_service_name($id);

    $status_cmd = "systemctl is-active " . escapeshellarg($service_name) . " 2>&1";
    $status = trim(shell_exec($status_cmd));

    return [
        'success' => true,
        'id' => $id,
        'name' => $config['output']['name'] ?? $id,
        'type' => $type,
        'service_name' => $service_name,
        'status' => $status === 'active' ? 'running' : 'stopped'
    ];
}
