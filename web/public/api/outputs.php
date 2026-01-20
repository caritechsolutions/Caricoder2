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

    case 'check_name':
        $name = $_GET['name'] ?? '';
        $exclude_id = $_GET['exclude'] ?? '';

        if (empty($name)) {
            json_response(['error' => 'Name required'], 400);
        }

        $id = sanitize_name_to_id($name);
        $exists = output_exists($id, $exclude_id);

        json_response([
            'available' => !$exists,
            'suggested_id' => $id
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
 * Create new output
 */
function create_output($data) {
    $name = trim($data['name'] ?? '');
    $id = !empty($data['id']) ? sanitize_name_to_id($data['id']) : sanitize_name_to_id($name);
    $type = $data['type'] ?? 'udp';

    if (empty($name)) {
        return ['success' => false, 'error' => 'Output name required'];
    }

    if (output_exists($id)) {
        return ['success' => false, 'error' => 'Output with this ID already exists'];
    }

    // Build configuration
    $config = [
        'output' => [
            'id' => $id,
            'name' => $name,
            'type' => $type,
            'enabled' => 'true'
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

    return ['success' => true, 'id' => $id, 'message' => 'Output created successfully'];
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

    // Stop service if running
    stop_output_service($id);

    // Load existing config and merge
    $config = parse_config($config_file);
    $type = $data['type'] ?? $config['output']['type'] ?? 'udp';

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

    // Update type-specific fields
    switch ($type) {
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

    // Stop service first
    stop_output_service($id);

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

    $service = "cari-output@{$id}";
    $cmd = "sudo /bin/systemctl start " . escapeshellarg($service) . " 2>&1";
    $output = shell_exec($cmd);

    // Check if started
    usleep(500000);
    $status_cmd = "systemctl is-active " . escapeshellarg($service) . " 2>&1";
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
    $service = "cari-output@{$id}";

    $cmd = "sudo /bin/systemctl stop " . escapeshellarg($service) . " 2>&1";
    shell_exec($cmd);

    usleep(500000);
    $status_cmd = "systemctl is-active " . escapeshellarg($service) . " 2>&1";
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
    $service = "cari-output@{$id}";

    $status_cmd = "systemctl is-active " . escapeshellarg($service) . " 2>&1";
    $status = trim(shell_exec($status_cmd));

    return [
        'success' => true,
        'id' => $id,
        'name' => $config['output']['name'] ?? $id,
        'type' => $config['output']['type'] ?? 'udp',
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

    // For SRT outputs, we could query connected clients count, etc.
    // For now, return basic status
    $service = "cari-output@{$id}";
    $status_cmd = "systemctl is-active " . escapeshellarg($service) . " 2>&1";
    $status = trim(shell_exec($status_cmd));

    return [
        'success' => true,
        'id' => $id,
        'name' => $config['output']['name'] ?? $id,
        'type' => $type,
        'status' => $status === 'active' ? 'running' : 'stopped'
    ];
}
