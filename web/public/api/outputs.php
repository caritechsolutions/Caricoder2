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
define('CARI_API_URL', 'http://127.0.0.1:8081');
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

    case 'available_sources':
    case 'available_buffers':  // Legacy alias
        // Get all available sources (inputs, transcoders, muxers) with their UDP output info
        $sources = get_available_sources();
        json_response(['sources' => $sources]);
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

    case 'rist_metrics':
        $id = $_GET['id'] ?? '';
        if (empty($id)) {
            json_response(['error' => 'Output ID required'], 400);
        }

        $metrics = get_rist_metrics($id);
        json_response($metrics);
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

    case 'clients':
        $id = $_GET['id'] ?? '';
        if (empty($id)) {
            json_response(['error' => 'Output ID required'], 400);
        }

        $clients = get_output_clients($id);
        json_response($clients);
        break;

    case 'client_info':
        $id = $_GET['id'] ?? '';
        $slot = $_GET['slot'] ?? '';
        if (empty($id) || $slot === '') {
            json_response(['error' => 'Output ID and slot required'], 400);
        }

        $client = get_output_client_info($id, intval($slot));
        json_response($client);
        break;

    case 'kick_client':
        $id = $_GET['id'] ?? $_POST['id'] ?? '';
        $slot = $_GET['slot'] ?? $_POST['slot'] ?? '';
        if (empty($id) || $slot === '') {
            json_response(['error' => 'Output ID and slot required'], 400);
        }

        $result = kick_output_client($id, intval($slot));
        json_response($result);
        break;

    default:
        json_response(['error' => 'Invalid action'], 400);
}

/**
 * Get all available sources (inputs, transcoders, muxers) with their UDP output info
 */
function get_available_sources() {
    $sources = [];

    // Get sources from inputs
    $inputs = get_service_list('inputs');
    foreach ($inputs as $input) {
        $id = $input['id'] ?? '';
        $name = $input['name'] ?? $id;
        if ($id) {
            // Get output address/port from config
            $config_file = CONFIG_PATH . '/inputs/' . $id . '.conf';
            $output_address = '';
            $output_port = '';
            if (file_exists($config_file)) {
                $config = parse_config($config_file);
                $output_address = $config['output']['address'] ?? '';
                $output_port = $config['output']['port'] ?? '';
            }
            $sources[] = [
                'source_id' => $id,
                'display_name' => $name,
                'source_type' => 'input',
                'output_address' => $output_address,
                'output_port' => $output_port,
                'status' => $input['status'] ?? 'unknown'
            ];
        }
    }

    // Get sources from transcoders
    $transcoders = get_service_list('transcoders');
    foreach ($transcoders as $transcoder) {
        $id = $transcoder['id'] ?? '';
        $name = $transcoder['name'] ?? $id;
        if ($id) {
            // Get output address/port from config
            $config_file = CONFIG_PATH . '/transcoders/' . $id . '.conf';
            $output_address = '';
            $output_port = '';
            if (file_exists($config_file)) {
                $config = parse_config($config_file);
                $output_address = $config['output']['address'] ?? '';
                $output_port = $config['output']['port'] ?? '';
            }
            $sources[] = [
                'source_id' => $id,
                'display_name' => $name,
                'source_type' => 'transcoder',
                'output_address' => $output_address,
                'output_port' => $output_port,
                'status' => $transcoder['status'] ?? 'unknown'
            ];
        }
    }

    // Get sources from muxers
    $muxers = get_service_list('muxers');
    foreach ($muxers as $muxer) {
        $id = $muxer['id'] ?? '';
        $name = $muxer['name'] ?? $id;
        if ($id) {
            // Get output address/port from config
            $config_file = CONFIG_PATH . '/muxers/' . $id . '.conf';
            $output_address = '';
            $output_port = '';
            if (file_exists($config_file)) {
                $config = parse_config($config_file);
                $output_address = $config['output']['address'] ?? '';
                $output_port = $config['output']['port'] ?? '';
            }
            $sources[] = [
                'source_id' => $id,
                'display_name' => $name,
                'source_type' => 'muxer',
                'output_address' => $output_address,
                'output_port' => $output_port,
                'status' => $muxer['status'] ?? 'unknown'
            ];
        }
    }

    return $sources;
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
 * Generate systemd service file for output via backend API
 */
function generate_output_service_file($id, $type, $name) {
    $service_name = $id . '-output-' . $type;
    $config_path = CONFIG_PATH . '/outputs/' . $id . '.conf';

    $service_content = <<<EOT
[Unit]
Description=CariTranscoder Output - {$name}
Documentation=https://github.com/caritechsolutions/caritranscoder
After=network.target
Wants=network-online.target
StartLimitIntervalSec=60
StartLimitBurst=5

[Service]
Type=simple
User=root
Group=root

ExecStart=/usr/local/bin/cari-output --config {$config_path}
ExecReload=/bin/kill -HUP \$MAINPID

Restart=always
RestartSec=5

LimitNOFILE=65535
LimitNPROC=4096

StandardOutput=journal
StandardError=journal
SyslogIdentifier={$service_name}

[Install]
WantedBy=multi-user.target
EOT;

    // Use backend API to create service file
    $result = call_cari_api('/service/file/create', 'POST', [
        'service_name' => $service_name,
        'content' => $service_content
    ]);

    if (isset($result['success']) && $result['success']) {
        return $service_name;
    }

    // Log error but still return service name
    error_log("Failed to create service file: " . json_encode($result));
    return $service_name;
}

/**
 * Delete systemd service file for output via backend API
 */
function delete_output_service_file($id, $type) {
    $service_name = $id . '-output-' . $type;

    // Use backend API to delete service file
    $result = call_cari_api('/service/file/delete', 'POST', [
        'service_name' => $service_name,
        'content' => ''  // Not needed for delete but required by model
    ]);

    return isset($result['success']) && $result['success'];
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

    if ($type === 'rist') {
        return create_rist_output($data, $id, $name, $service_name);
    }

    // SRT output (default)
    // Auto-assign API port (SRT port + 1000)
    $srt_port = intval($data['srt_port'] ?? 4900);
    $api_port = $srt_port + 1000;

    // Build configuration
    $config = [
        'output' => [
            'id' => $id,
            'name' => $name,
            'type' => 'srt',
            'enabled' => 'true',
            'service_name' => $service_name,
            'api_port' => strval($api_port)
        ],
        'input' => [
            'address' => $data['input_address'] ?? '',
            'port' => $data['input_port'] ?? '5000',
            'interface' => $data['input_interface'] ?? ''
        ],
        'destination_srt' => [
            'listen_address' => $data['srt_listen_address'] ?? '0.0.0.0',
            'listen_port' => $data['srt_port'] ?? '4900',
            'latency' => $data['srt_latency'] ?? '120',
            'passphrase' => $data['srt_passphrase'] ?? '',
            'pbkeylen' => $data['srt_pbkeylen'] ?? '0',
            'streamid' => $data['srt_streamid'] ?? '',
            'max_clients' => $data['srt_max_clients'] ?? '10'
        ]
    ];

    // Save configuration
    $config_file = CONFIG_PATH . '/outputs/' . $id . '.conf';
    if (!save_config($config_file, $config)) {
        return ['success' => false, 'error' => 'Failed to save configuration'];
    }

    // Generate systemd service file
    generate_output_service_file($id, 'srt', $name);

    return ['success' => true, 'id' => $id, 'service_name' => $service_name, 'message' => 'Output created successfully'];
}

/**
 * Create RIST output
 */
function create_rist_output($data, $id, $name, $service_name) {
    // Log for debugging
    error_log("create_rist_output called: id={$id}, name={$name}");

    // Assign metrics port (use provided or default to 9100 + offset based on RIST port)
    $rist_port = intval($data['rist_port'] ?? 5001);
    $metrics_port = intval($data['metrics_port'] ?? (9100 + ($rist_port % 1000)));

    // Build configuration
    $config = [
        'output' => [
            'id' => $id,
            'name' => $name,
            'type' => 'rist',
            'enabled' => 'true',
            'service_name' => $service_name,
            'metrics_port' => strval($metrics_port)
        ],
        'input' => [
            'address' => $data['input_address'] ?? '',
            'port' => $data['input_port'] ?? '5000',
            'interface' => $data['input_interface'] ?? ''
        ],
        'destination_rist' => [
            'mode' => $data['rist_mode'] ?? 'caller',
            'address' => ltrim($data['rist_address'] ?? '', '@'), // Strip any leading @
            'port' => $data['rist_port'] ?? '5001',
            'profile' => $data['rist_profile'] ?? '1',
            'buffer' => $data['rist_buffer'] ?? '250',
            'encryption' => $data['rist_encryption'] ?? '0',
            'secret' => $data['rist_secret'] ?? '',
            'cname' => $data['rist_cname'] ?? '',
            'npd' => ($data['rist_npd'] ?? '0') === '1' ? 'true' : 'false',
            'bandwidth' => $data['rist_bandwidth'] ?? '0',
            'congestion_control' => $data['rist_congestion'] ?? '1',
            'log_level' => $data['rist_log_level'] ?? '6'
        ]
    ];

    // Ensure outputs directory exists
    $output_dir = CONFIG_PATH . '/outputs';
    error_log("Output dir: {$output_dir}");

    if (!is_dir($output_dir)) {
        error_log("Creating output dir: {$output_dir}");
        if (!mkdir($output_dir, 0755, true)) {
            error_log("Failed to create output dir");
            return ['success' => false, 'error' => 'Failed to create outputs directory'];
        }
    }

    // Save configuration
    $config_file = $output_dir . '/' . $id . '.conf';
    error_log("Saving config to: {$config_file}");

    $save_result = save_config($config_file, $config);
    error_log("Save result: " . ($save_result ? 'true' : 'false'));

    if (!$save_result) {
        return ['success' => false, 'error' => 'Failed to save configuration to: ' . $config_file];
    }

    // Verify config was saved
    if (!file_exists($config_file)) {
        error_log("Config file not found after save: {$config_file}");
        return ['success' => false, 'error' => 'Config file not created: ' . $config_file];
    }

    error_log("Config file verified: {$config_file}");

    // Generate systemd service file for ristsender
    generate_rist_service_file($id, $name, $config);

    return ['success' => true, 'id' => $id, 'service_name' => $service_name, 'config_file' => $config_file, 'message' => 'RIST output created successfully'];
}

/**
 * Generate systemd service file for RIST output (ristsender)
 */
function generate_rist_service_file($id, $name, $config) {
    $service_name = $id . '-output-rist';

    // Build ristsender command
    $input = $config['input'] ?? [];
    $dest = $config['destination_rist'] ?? [];
    $output_cfg = $config['output'] ?? [];

    // Input URL
    $input_addr = $input['address'] ?? '';
    $input_port = $input['port'] ?? '5000';
    $input_iface = $input['interface'] ?? '';

    if (!empty($input_addr)) {
        // Multicast
        $input_url = "udp://{$input_addr}:{$input_port}";
        if (!empty($input_iface)) {
            $input_url .= "?miface={$input_iface}";
        }
    } else {
        // Unicast - listen on all interfaces
        $input_url = "udp://0.0.0.0:{$input_port}";
    }

    // Output URL
    $mode = $dest['mode'] ?? 'caller';
    $rist_addr = ltrim($dest['address'] ?? '', '@'); // Strip any leading @ from address
    $rist_port = $dest['port'] ?? '5001';
    $profile = $dest['profile'] ?? '1';
    $buffer = $dest['buffer'] ?? '250';
    $encryption = $dest['encryption'] ?? '0';
    $secret = $dest['secret'] ?? '';
    $cname = $dest['cname'] ?? $name;
    $npd = ($dest['npd'] ?? 'false') === 'true';
    $bandwidth = $dest['bandwidth'] ?? '0';
    $congestion = $dest['congestion_control'] ?? '1';
    $log_level = $dest['log_level'] ?? '6';
    $metrics_port = $output_cfg['metrics_port'] ?? '9100';

    // Build RIST URL based on mode
    if ($mode === 'listener') {
        // Listener mode - use @ prefix
        $output_url = "rist://@{$rist_addr}:{$rist_port}";
    } else {
        // Caller mode - standard URL
        $output_url = "rist://{$rist_addr}:{$rist_port}";
    }

    // Add URL parameters
    $params = [];
    if (intval($buffer) > 0) {
        $params[] = "buffer={$buffer}";
    }
    if (!empty($cname)) {
        $params[] = "cname=" . urlencode($cname);
    }
    if (intval($encryption) > 0 && !empty($secret)) {
        $params[] = "aes-type={$encryption}";
        $params[] = "secret=" . urlencode($secret);
    }
    if (intval($bandwidth) > 0) {
        $params[] = "bandwidth={$bandwidth}";
    }
    if (intval($congestion) >= 0) {
        $params[] = "congestion-control={$congestion}";
    }

    if (!empty($params)) {
        $output_url .= '?' . implode('&', $params);
    }

    // Build command arguments
    $cmd_args = [];
    $cmd_args[] = "--inputurl \"{$input_url}\"";
    $cmd_args[] = "--outputurl \"{$output_url}\"";
    $cmd_args[] = "--profile {$profile}";
    $cmd_args[] = "--verbose-level {$log_level}";
    $cmd_args[] = "--statsinterval 1000";

    if ($npd) {
        $cmd_args[] = "--null-packet-deletion";
    }

    // Enable metrics HTTP server
    $cmd_args[] = "--metrics-http";
    $cmd_args[] = "--metrics-port {$metrics_port}";
    $cmd_args[] = "--metrics-ip 127.0.0.1";

    $cmd_line = implode(" \\\n    ", $cmd_args);

    $service_content = <<<EOT
[Unit]
Description=CariTranscoder RIST Output - {$name}
Documentation=https://github.com/caritechsolutions/caritranscoder
After=network.target
Wants=network-online.target
StartLimitIntervalSec=60
StartLimitBurst=5

[Service]
Type=simple
User=root
Group=root

ExecStart=/usr/bin/ristsender \\
    {$cmd_line}

Restart=always
RestartSec=5

LimitNOFILE=65535
LimitNPROC=4096

StandardOutput=journal
StandardError=journal
SyslogIdentifier={$service_name}

[Install]
WantedBy=multi-user.target
EOT;

    // Use backend API to create service file
    $result = call_cari_api('/service/file/create', 'POST', [
        'service_name' => $service_name,
        'content' => $service_content
    ]);

    if (isset($result['success']) && $result['success']) {
        return $service_name;
    }

    error_log("Failed to create RIST service file: " . json_encode($result));
    return $service_name;
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
    $type = $config['output']['type'] ?? 'srt';

    // Stop service if running
    stop_output_service($id);

    // Update basic fields
    if (!empty($data['name'])) {
        $config['output']['name'] = $data['name'];
    }

    // Update UDP input fields
    if (!isset($config['input'])) $config['input'] = [];
    if (isset($data['input_address'])) $config['input']['address'] = $data['input_address'];
    if (!empty($data['input_port'])) $config['input']['port'] = $data['input_port'];
    if (isset($data['input_interface'])) $config['input']['interface'] = $data['input_interface'];

    if ($type === 'rist') {
        // Update RIST output fields
        $config['output']['service_name'] = $id . '-output-rist';

        if (!isset($config['destination_rist'])) $config['destination_rist'] = [];
        if (isset($data['rist_mode'])) $config['destination_rist']['mode'] = $data['rist_mode'];
        if (isset($data['rist_address'])) $config['destination_rist']['address'] = ltrim($data['rist_address'], '@');
        if (!empty($data['rist_port'])) $config['destination_rist']['port'] = $data['rist_port'];
        if (isset($data['rist_profile'])) $config['destination_rist']['profile'] = $data['rist_profile'];
        if (isset($data['rist_buffer'])) $config['destination_rist']['buffer'] = $data['rist_buffer'];
        if (isset($data['rist_encryption'])) $config['destination_rist']['encryption'] = $data['rist_encryption'];
        if (isset($data['rist_secret'])) $config['destination_rist']['secret'] = $data['rist_secret'];
        if (isset($data['rist_cname'])) $config['destination_rist']['cname'] = $data['rist_cname'];
        if (isset($data['rist_npd'])) $config['destination_rist']['npd'] = $data['rist_npd'] === '1' ? 'true' : 'false';
        if (isset($data['rist_bandwidth'])) $config['destination_rist']['bandwidth'] = $data['rist_bandwidth'];
        if (isset($data['rist_congestion'])) $config['destination_rist']['congestion_control'] = $data['rist_congestion'];
        if (isset($data['rist_log_level'])) $config['destination_rist']['log_level'] = $data['rist_log_level'];

        if (!save_config($config_file, $config)) {
            return ['success' => false, 'error' => 'Failed to save configuration'];
        }

        // Regenerate service file
        $name = $config['output']['name'] ?? $id;
        generate_rist_service_file($id, $name, $config);
    } else {
        // SRT output
        $config['output']['type'] = 'srt';
        $config['output']['service_name'] = $id . '-output-srt';

        // Update SRT output fields
        if (!isset($config['destination_srt'])) $config['destination_srt'] = [];
        if (!empty($data['srt_listen_address'])) $config['destination_srt']['listen_address'] = $data['srt_listen_address'];
        if (!empty($data['srt_port'])) $config['destination_srt']['listen_port'] = $data['srt_port'];
        if (!empty($data['srt_latency'])) $config['destination_srt']['latency'] = $data['srt_latency'];
        if (isset($data['srt_passphrase'])) $config['destination_srt']['passphrase'] = $data['srt_passphrase'];
        if (isset($data['srt_pbkeylen'])) $config['destination_srt']['pbkeylen'] = $data['srt_pbkeylen'];
        if (isset($data['srt_streamid'])) $config['destination_srt']['streamid'] = $data['srt_streamid'];
        if (!empty($data['srt_max_clients'])) $config['destination_srt']['max_clients'] = $data['srt_max_clients'];

        if (!save_config($config_file, $config)) {
            return ['success' => false, 'error' => 'Failed to save configuration'];
        }

        // Generate/update systemd service file
        $name = $config['output']['name'] ?? $id;
        generate_output_service_file($id, 'srt', $name);
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
 * Call the CariTranscoder backend API
 */
function call_cari_api($endpoint, $method = 'GET', $data = null) {
    $url = CARI_API_URL . $endpoint;

    $options = [
        'http' => [
            'method' => $method,
            'timeout' => 10,
            'ignore_errors' => true,
            'header' => "Content-Type: application/json\r\n"
        ]
    ];

    if ($data !== null && in_array($method, ['POST', 'PUT', 'PATCH'])) {
        $options['http']['content'] = json_encode($data);
    }

    $context = stream_context_create($options);
    $response = @file_get_contents($url, false, $context);

    if ($response === false) {
        return ['success' => false, 'error' => 'Failed to connect to CariTranscoder API'];
    }

    return json_decode($response, true) ?: ['success' => false, 'error' => 'Invalid API response'];
}

/**
 * Start output service via backend API
 */
function start_output_service($id) {
    $id = preg_replace('/[^a-zA-Z0-9_-]/', '', $id);
    $config_file = CONFIG_PATH . '/outputs/' . $id . '.conf';

    if (!file_exists($config_file)) {
        return ['success' => false, 'error' => 'Output not found'];
    }

    $config = parse_config($config_file);
    $type = $config['output']['type'] ?? 'srt';
    $service_name = $id . '-output-' . $type;

    // Use same endpoint as muxers
    $result = call_cari_api('/service/control', 'POST', [
        'action' => 'start',
        'service_name' => $service_name
    ]);

    if (isset($result['success']) && $result['success']) {
        return ['success' => true, 'message' => 'Output started'];
    }

    return ['success' => false, 'error' => $result['error'] ?? $result['stderr'] ?? 'Failed to start output'];
}

/**
 * Stop output service via backend API
 */
function stop_output_service($id) {
    $id = preg_replace('/[^a-zA-Z0-9_-]/', '', $id);
    $config_file = CONFIG_PATH . '/outputs/' . $id . '.conf';

    if (!file_exists($config_file)) {
        return ['success' => true, 'message' => 'No service to stop'];
    }

    $config = parse_config($config_file);
    $type = $config['output']['type'] ?? 'srt';
    $service_name = $id . '-output-' . $type;

    // Use same endpoint as muxers
    $result = call_cari_api('/service/control', 'POST', [
        'action' => 'stop',
        'service_name' => $service_name
    ]);

    if (isset($result['success']) && $result['success']) {
        return ['success' => true, 'message' => 'Output stopped'];
    }

    return ['success' => false, 'error' => $result['error'] ?? $result['stderr'] ?? 'Failed to stop output'];
}

/**
 * Get output status via backend API
 */
function get_output_status($id) {
    $id = preg_replace('/[^a-zA-Z0-9_-]/', '', $id);
    $config_file = CONFIG_PATH . '/outputs/' . $id . '.conf';

    if (!file_exists($config_file)) {
        return ['success' => false, 'error' => 'Output not found'];
    }

    $config = parse_config($config_file);
    $type = $config['output']['type'] ?? 'srt';
    $service_name = get_output_service_name($id);

    // Call backend API to get status
    $result = call_cari_api("/output/{$id}/status?output_type={$type}", 'GET');

    $is_active = isset($result['active']) && $result['active'];

    return [
        'success' => true,
        'id' => $id,
        'name' => $config['output']['name'] ?? $id,
        'type' => $type,
        'service_name' => $service_name,
        'status' => $is_active ? 'running' : 'stopped'
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

    // Get status via backend API
    $result = call_cari_api("/output/{$id}/status?output_type={$type}", 'GET');
    $is_active = isset($result['active']) && $result['active'];

    return [
        'success' => true,
        'id' => $id,
        'name' => $config['output']['name'] ?? $id,
        'type' => $type,
        'service_name' => $service_name,
        'status' => $is_active ? 'running' : 'stopped'
    ];
}

/**
 * Get API port for output from config
 */
function get_output_api_port($id) {
    $id = preg_replace('/[^a-zA-Z0-9_-]/', '', $id);
    $config_file = CONFIG_PATH . '/outputs/' . $id . '.conf';

    if (!file_exists($config_file)) {
        return null;
    }

    $config = parse_config($config_file);
    return $config['output']['api_port'] ?? null;
}

/**
 * Call output's HTTP API
 */
function call_output_api($id, $endpoint, $method = 'GET') {
    $api_port = get_output_api_port($id);

    if (!$api_port) {
        return ['success' => false, 'error' => 'No API port configured for this output'];
    }

    $url = "http://127.0.0.1:{$api_port}{$endpoint}";
    $ctx = stream_context_create([
        'http' => [
            'method' => $method,
            'timeout' => 5,
            'ignore_errors' => true
        ]
    ]);

    $response = @file_get_contents($url, false, $ctx);

    if ($response === false) {
        return ['success' => false, 'error' => 'Cannot connect to output API', 'status' => 'offline'];
    }

    return json_decode($response, true) ?: ['success' => false, 'error' => 'Invalid API response'];
}

/**
 * Get connected clients for an output
 */
function get_output_clients($id) {
    $id = preg_replace('/[^a-zA-Z0-9_-]/', '', $id);
    $config_file = CONFIG_PATH . '/outputs/' . $id . '.conf';

    if (!file_exists($config_file)) {
        return ['success' => false, 'error' => 'Output not found'];
    }

    $config = parse_config($config_file);
    $api_port = $config['output']['api_port'] ?? null;

    if (!$api_port) {
        return ['success' => false, 'error' => 'No API port configured - add api_port to output config'];
    }

    $result = call_output_api($id, '/clients');

    if (isset($result['success']) && $result['success']) {
        return [
            'success' => true,
            'id' => $id,
            'name' => $config['output']['name'] ?? $id,
            'client_count' => $result['client_count'] ?? 0,
            'max_clients' => $result['max_clients'] ?? 10,
            'clients' => $result['clients'] ?? []
        ];
    }

    return $result;
}

/**
 * Get info for a specific client
 */
function get_output_client_info($id, $slot) {
    $id = preg_replace('/[^a-zA-Z0-9_-]/', '', $id);
    $config_file = CONFIG_PATH . '/outputs/' . $id . '.conf';

    if (!file_exists($config_file)) {
        return ['success' => false, 'error' => 'Output not found'];
    }

    return call_output_api($id, "/client/{$slot}");
}

/**
 * Kick a client from an output
 */
function kick_output_client($id, $slot) {
    $id = preg_replace('/[^a-zA-Z0-9_-]/', '', $id);
    $config_file = CONFIG_PATH . '/outputs/' . $id . '.conf';

    if (!file_exists($config_file)) {
        return ['success' => false, 'error' => 'Output not found'];
    }

    return call_output_api($id, "/client/{$slot}/kick", 'POST');
}

/**
 * Get RIST metrics from ristsender's Prometheus endpoint
 */
function get_rist_metrics($id) {
    $id = preg_replace('/[^a-zA-Z0-9_-]/', '', $id);
    $config_file = CONFIG_PATH . '/outputs/' . $id . '.conf';

    if (!file_exists($config_file)) {
        return ['success' => false, 'error' => 'Output not found'];
    }

    $config = parse_config($config_file);

    if (($config['output']['type'] ?? '') !== 'rist') {
        return ['success' => false, 'error' => 'Not a RIST output'];
    }

    $metrics_port = $config['output']['metrics_port'] ?? '9100';
    $url = "http://127.0.0.1:{$metrics_port}/metrics";

    $ctx = stream_context_create([
        'http' => [
            'method' => 'GET',
            'timeout' => 5,
            'ignore_errors' => true
        ]
    ]);

    $response = @file_get_contents($url, false, $ctx);

    if ($response === false) {
        return ['success' => false, 'error' => 'Cannot connect to RIST metrics server', 'status' => 'offline'];
    }

    // Parse Prometheus-format metrics
    $metrics = parse_prometheus_metrics($response);

    return [
        'success' => true,
        'id' => $id,
        'name' => $config['output']['name'] ?? $id,
        'type' => 'rist',
        'raw' => $response,
        'metrics' => $metrics
    ];
}

/**
 * Parse Prometheus-format metrics into structured data
 */
function parse_prometheus_metrics($text) {
    $metrics = [];
    $lines = explode("\n", $text);

    foreach ($lines as $line) {
        $line = trim($line);

        // Skip empty lines and comments
        if (empty($line) || $line[0] === '#') {
            continue;
        }

        // Parse metric line: metric_name{labels} value
        if (preg_match('/^([a-zA-Z_][a-zA-Z0-9_]*)\{([^}]*)\}\s+(.+)$/', $line, $matches)) {
            $name = $matches[1];
            $labels_str = $matches[2];
            $value = $matches[3];

            // Parse labels
            $labels = [];
            if (!empty($labels_str)) {
                preg_match_all('/([a-zA-Z_][a-zA-Z0-9_]*)="([^"]*)"/', $labels_str, $label_matches, PREG_SET_ORDER);
                foreach ($label_matches as $lm) {
                    $labels[$lm[1]] = $lm[2];
                }
            }

            $metrics[] = [
                'name' => $name,
                'labels' => $labels,
                'value' => is_numeric($value) ? floatval($value) : $value
            ];
        } elseif (preg_match('/^([a-zA-Z_][a-zA-Z0-9_]*)\s+(.+)$/', $line, $matches)) {
            // Metric without labels
            $metrics[] = [
                'name' => $matches[1],
                'labels' => [],
                'value' => is_numeric($matches[2]) ? floatval($matches[2]) : $matches[2]
            ];
        }
    }

    // Group metrics by type for easier consumption
    $grouped = [
        'sender' => [],
        'receiver' => [],
        'peer' => [],
        'flow' => [],
        'other' => []
    ];

    foreach ($metrics as $m) {
        $name = $m['name'];
        if (strpos($name, 'rist_sender_') === 0) {
            $grouped['sender'][] = $m;
        } elseif (strpos($name, 'rist_receiver_') === 0) {
            $grouped['receiver'][] = $m;
        } elseif (strpos($name, 'rist_peer_') === 0) {
            $grouped['peer'][] = $m;
        } elseif (strpos($name, 'rist_flow_') === 0) {
            $grouped['flow'][] = $m;
        } else {
            $grouped['other'][] = $m;
        }
    }

    return $grouped;
}
