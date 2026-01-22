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

/**
 * Find an available port in a given range
 * Checks both system ports and existing output configs
 */
function find_available_metrics_port($start_port = 9100, $end_port = 9199) {
    // Get list of ports already used by existing outputs
    $used_ports = [];
    $outputs_dir = CONFIG_PATH . '/outputs';
    if (is_dir($outputs_dir)) {
        $files = glob($outputs_dir . '/*.conf');
        foreach ($files as $file) {
            $config = parse_config($file);
            if (!empty($config['output']['metrics_port'])) {
                $used_ports[] = intval($config['output']['metrics_port']);
            }
        }
    }

    // Find first available port
    for ($port = $start_port; $port <= $end_port; $port++) {
        // Skip if already used by another output
        if (in_array($port, $used_ports)) {
            continue;
        }

        // Check if port is actually available on the system
        $socket = @fsockopen('127.0.0.1', $port, $errno, $errstr, 0.1);
        if ($socket === false) {
            // Port is available (connection refused = not in use)
            return $port;
        }
        fclose($socket);
    }

    // Fallback if no port found in range
    return $start_port;
}

/**
 * Get geolocation info for an IP address using ip-api.com
 * Results are cached in /tmp to avoid rate limits
 */
function get_ip_geolocation($ip) {
    // Skip private/local IPs
    if (filter_var($ip, FILTER_VALIDATE_IP, FILTER_FLAG_NO_PRIV_RANGE | FILTER_FLAG_NO_RES_RANGE) === false) {
        return ['country' => 'Local', 'countryCode' => 'LO', 'city' => '', 'isp' => 'Private Network'];
    }

    // Check cache first (cache for 24 hours)
    $cache_dir = '/tmp/geoip_cache';
    if (!is_dir($cache_dir)) {
        @mkdir($cache_dir, 0755, true);
    }
    $cache_file = $cache_dir . '/' . md5($ip) . '.json';

    if (file_exists($cache_file) && (time() - filemtime($cache_file)) < 86400) {
        $cached = json_decode(file_get_contents($cache_file), true);
        if ($cached) {
            return $cached;
        }
    }

    // Query ip-api.com (free, no API key needed, 45 req/min limit)
    $url = "http://ip-api.com/json/{$ip}?fields=status,message,country,countryCode,city,isp";
    $ctx = stream_context_create([
        'http' => [
            'timeout' => 3,
            'ignore_errors' => true
        ]
    ]);

    $response = @file_get_contents($url, false, $ctx);
    if ($response === false) {
        return ['country' => 'Unknown', 'countryCode' => '??', 'city' => '', 'isp' => ''];
    }

    $data = json_decode($response, true);
    if (!$data || ($data['status'] ?? '') !== 'success') {
        return ['country' => 'Unknown', 'countryCode' => '??', 'city' => '', 'isp' => ''];
    }

    $result = [
        'country' => $data['country'] ?? 'Unknown',
        'countryCode' => $data['countryCode'] ?? '??',
        'city' => $data['city'] ?? '',
        'isp' => $data['isp'] ?? ''
    ];

    // Cache the result
    @file_put_contents($cache_file, json_encode($result));

    return $result;
}

/**
 * Extract IP address from various URL formats
 */
function extract_ip_from_url($url) {
    // Handle formats like: rist://192.168.1.1:5001, 192.168.1.1:5000, etc.
    $url = preg_replace('/^[a-z]+:\/\//', '', $url); // Remove protocol
    $url = preg_replace('/@/', '', $url); // Remove @ for listener mode
    $parts = explode(':', $url);
    $ip = $parts[0] ?? '';

    // Validate it's an IP
    if (filter_var($ip, FILTER_VALIDATE_IP)) {
        return $ip;
    }
    return null;
}

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

    case 'geoip':
        // Get geolocation for one or multiple IPs
        $ip = $_GET['ip'] ?? '';
        $ips = $_GET['ips'] ?? '';

        if (!empty($ips)) {
            // Multiple IPs (comma-separated)
            $ip_list = array_filter(array_map('trim', explode(',', $ips)));
            $results = [];
            foreach ($ip_list as $single_ip) {
                $results[$single_ip] = get_ip_geolocation($single_ip);
            }
            json_response(['success' => true, 'results' => $results]);
        } elseif (!empty($ip)) {
            // Single IP
            $result = get_ip_geolocation($ip);
            json_response(['success' => true, 'ip' => $ip, 'geo' => $result]);
        } else {
            json_response(['error' => 'IP address required'], 400);
        }
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

    case 'http_stats':
        $id = $_GET['id'] ?? '';
        if (empty($id)) {
            json_response(['error' => 'Output ID required'], 400);
        }

        $stats = get_http_stats($id);
        json_response($stats);
        break;

    case 'hls_stats':
        $id = $_GET['id'] ?? '';
        if (empty($id)) {
            json_response(['error' => 'Output ID required'], 400);
        }

        $stats = get_hls_stats($id);
        json_response($stats);
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
    // Check both 'output' and 'general' sections for type (for consistency)
    $type = $config['output']['type'] ?? $config['general']['type'] ?? 'srt';
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

    if ($type === 'http') {
        return create_http_output($data, $id, $name, $service_name);
    }

    if ($type === 'hls') {
        return create_hls_output($data, $id, $name, $service_name);
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

    // Assign metrics port (use provided or find an available one)
    $metrics_port = intval($data['metrics_port'] ?? 0);
    if ($metrics_port === 0) {
        $metrics_port = find_available_metrics_port(9100, 9199);
    }
    error_log("Using metrics port: {$metrics_port}");

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

    if ($npd) {
        $cmd_args[] = "--null-packet-deletion";
    }

    // Enable metrics HTTP server
    $cmd_args[] = "-M";
    $cmd_args[] = "--metrics-http";
    $cmd_args[] = "--metrics-port={$metrics_port}";
    $cmd_args[] = "-S 1000";

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

ExecStart=/usr/local/bin/ristsender \\
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
 * Create HTTP MPEG-TS output
 */
function create_http_output($data, $id, $name, $service_name) {
    // Log for debugging
    error_log("create_http_output called: id={$id}, name={$name}");

    // Build configuration
    $config = [
        'output' => [
            'id' => $id,
            'name' => $name,
            'type' => 'http',
            'enabled' => 'true',
            'service_name' => $service_name
        ],
        'input' => [
            'address' => $data['input_address'] ?? '',
            'port' => $data['input_port'] ?? '5000',
            'interface' => $data['input_interface'] ?? ''
        ],
        'destination_http' => [
            'listen_address' => $data['http_listen_address'] ?? '0.0.0.0',
            'listen_port' => $data['http_port'] ?? '8888',
            'stream_path' => $data['http_stream_path'] ?? '/stream',
            'stats_path' => $data['http_stats_path'] ?? '/stats',
            'mime_type' => $data['http_mime_type'] ?? 'video/mp2t',
            'chunked_encoding' => ($data['http_chunked'] ?? '0') === '1' ? 'true' : 'false'
        ]
    ];

    // Ensure outputs directory exists
    $output_dir = CONFIG_PATH . '/outputs';
    if (!is_dir($output_dir)) {
        if (!mkdir($output_dir, 0755, true)) {
            return ['success' => false, 'error' => 'Failed to create outputs directory'];
        }
    }

    // Save configuration
    $config_file = $output_dir . '/' . $id . '.conf';
    if (!save_config($config_file, $config)) {
        return ['success' => false, 'error' => 'Failed to save configuration'];
    }

    // Generate systemd service file
    generate_http_service_file($id, $name, $config);

    return ['success' => true, 'id' => $id, 'service_name' => $service_name, 'config_file' => $config_file, 'message' => 'HTTP output created successfully'];
}

/**
 * Generate systemd service file for HTTP MPEG-TS output
 */
function generate_http_service_file($id, $name, $config) {
    $service_name = $id . '-output-http';

    // Build http_ts_server command
    $input = $config['input'] ?? [];
    $dest = $config['destination_http'] ?? [];

    // Input URL
    $input_addr = $input['address'] ?? '';
    $input_port = $input['port'] ?? '5000';

    // Build UDP input string for http_ts_server
    if (!empty($input_addr)) {
        $udp_input = "{$input_addr}:{$input_port}";
    } else {
        $udp_input = ":{$input_port}";
    }

    // HTTP settings
    $http_port = $dest['listen_port'] ?? '8888';
    $stream_path = $dest['stream_path'] ?? '/stream';
    $stats_path = $dest['stats_path'] ?? '/stats';
    $mime_type = $dest['mime_type'] ?? 'video/mp2t';
    $chunked = ($dest['chunked_encoding'] ?? 'false') === 'true';

    // Build command arguments
    $cmd_args = [];
    $cmd_args[] = "-i \"{$udp_input}\"";
    $cmd_args[] = "-p {$http_port}";
    $cmd_args[] = "-s \"{$stream_path}\"";
    $cmd_args[] = "-a \"{$stats_path}\"";
    $cmd_args[] = "-m \"{$mime_type}\"";

    if ($chunked) {
        $cmd_args[] = "-c";
    }

    $cmd_args[] = "-v";

    $cmd_line = implode(" \\\n    ", $cmd_args);

    $service_content = <<<EOT
[Unit]
Description=CariTranscoder HTTP MPEG-TS Output - {$name}
Documentation=https://github.com/caritechsolutions/caritranscoder
After=network.target
Wants=network-online.target
StartLimitIntervalSec=60
StartLimitBurst=5

[Service]
Type=simple
User=root
Group=root

ExecStart=/usr/local/bin/http_ts_server \\
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

    error_log("Failed to create HTTP service file: " . json_encode($result));
    return $service_name;
}

/**
 * Create HLS output
 */
function create_hls_output($data, $id, $name, $service_name) {
    error_log("create_hls_output called: id={$id}, name={$name}");

    // Auto-generate output directory if not provided
    $output_dir = $data['hls_output_dir'] ?? '';
    if (empty(trim($output_dir))) {
        $output_dir = '/var/www/caritrans/public/hls/' . $id;
    }

    // Build configuration
    $config = [
        'output' => [
            'id' => $id,
            'name' => $name,
            'type' => 'hls',
            'enabled' => 'true',
            'service_name' => $service_name
        ],
        'input' => [
            'address' => $data['input_address'] ?? '',
            'port' => $data['input_port'] ?? '5000',
            'interface' => $data['input_interface'] ?? ''
        ],
        'destination_hls' => [
            'http_port' => $data['hls_port'] ?? '8080',
            'output_dir' => $output_dir,
            'segment_duration' => $data['hls_segment_duration'] ?? '2',
            'segment_count' => $data['hls_segment_count'] ?? '5',
            'variants' => $data['hls_variants'] ?? '1'
        ]
    ];

    // Ensure outputs directory exists
    $output_dir = CONFIG_PATH . '/outputs';
    if (!is_dir($output_dir)) {
        if (!mkdir($output_dir, 0755, true)) {
            return ['success' => false, 'error' => 'Failed to create outputs directory'];
        }
    }

    // Save configuration
    $config_file = $output_dir . '/' . $id . '.conf';
    if (!save_config($config_file, $config)) {
        return ['success' => false, 'error' => 'Failed to save configuration'];
    }

    // Generate systemd service file
    generate_hls_service_file($id, $name, $config);

    return ['success' => true, 'id' => $id, 'service_name' => $service_name, 'config_file' => $config_file, 'message' => 'HLS output created successfully'];
}

/**
 * Generate systemd service file for HLS output
 */
function generate_hls_service_file($id, $name, $config) {
    $service_name = $id . '-output-hls';

    $input = $config['input'] ?? [];
    $dest = $config['destination_hls'] ?? [];

    // Input settings
    $input_addr = $input['address'] ?? '';
    $input_port = $input['port'] ?? '5000';

    // Build UDP input string
    if (!empty($input_addr)) {
        $udp_input = "{$input_addr}:{$input_port}";
    } else {
        $udp_input = ":{$input_port}";
    }

    // HLS settings
    $http_port = $dest['http_port'] ?? '8080';
    $output_dir = $dest['output_dir'] ?? '/var/www/caritrans/public/hls/' . $id;
    $segment_duration = $dest['segment_duration'] ?? '2';
    $segment_count = $dest['segment_count'] ?? '5';
    $variants = $dest['variants'] ?? '1';

    // Build command arguments
    $cmd_args = [];
    $cmd_args[] = "-i \"{$udp_input}\"";
    $cmd_args[] = "-p {$http_port}";
    $cmd_args[] = "-o \"{$output_dir}\"";
    $cmd_args[] = "-d {$segment_duration}";
    $cmd_args[] = "-n {$segment_count}";

    if (intval($variants) > 1) {
        $cmd_args[] = "-V {$variants}";
    }

    $cmd_args[] = "-v";

    $cmd_line = implode(" \\\n    ", $cmd_args);

    $service_content = <<<EOT
[Unit]
Description=CariTranscoder HLS Output - {$name}
Documentation=https://github.com/caritechsolutions/caritranscoder
After=network.target
Wants=network-online.target
StartLimitIntervalSec=60
StartLimitBurst=5

[Service]
Type=simple
User=root
Group=root

ExecStart=/usr/local/bin/hls_output \\
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

    error_log("Failed to create HLS service file: " . json_encode($result));
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
    // Check both 'output' and 'general' sections for type (for consistency)
    $type = $config['output']['type'] ?? $config['general']['type'] ?? 'srt';

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
    } elseif ($type === 'http') {
        // Update HTTP output fields
        $config['output']['service_name'] = $id . '-output-http';

        if (!isset($config['destination_http'])) $config['destination_http'] = [];
        if (isset($data['http_listen_address'])) $config['destination_http']['listen_address'] = $data['http_listen_address'];
        if (!empty($data['http_port'])) $config['destination_http']['listen_port'] = $data['http_port'];
        if (isset($data['http_stream_path'])) $config['destination_http']['stream_path'] = $data['http_stream_path'];
        if (isset($data['http_stats_path'])) $config['destination_http']['stats_path'] = $data['http_stats_path'];
        if (isset($data['http_mime_type'])) $config['destination_http']['mime_type'] = $data['http_mime_type'];
        // Checkbox sends value only when checked, so treat missing as unchecked
        $config['destination_http']['chunked_encoding'] = (isset($data['http_chunked']) && $data['http_chunked'] === '1') ? 'true' : 'false';

        if (!save_config($config_file, $config)) {
            return ['success' => false, 'error' => 'Failed to save configuration'];
        }

        // Regenerate service file
        $name = $config['output']['name'] ?? $id;
        generate_http_service_file($id, $name, $config);
    } elseif ($type === 'hls') {
        // Update HLS output fields
        $config['output']['service_name'] = $id . '-output-hls';

        if (!isset($config['destination_hls'])) $config['destination_hls'] = [];
        if (!empty($data['hls_port'])) $config['destination_hls']['http_port'] = $data['hls_port'];
        if (isset($data['hls_output_dir'])) $config['destination_hls']['output_dir'] = $data['hls_output_dir'];
        if (!empty($data['hls_segment_duration'])) $config['destination_hls']['segment_duration'] = $data['hls_segment_duration'];
        if (!empty($data['hls_segment_count'])) $config['destination_hls']['segment_count'] = $data['hls_segment_count'];
        if (!empty($data['hls_variants'])) $config['destination_hls']['variants'] = $data['hls_variants'];

        if (!save_config($config_file, $config)) {
            return ['success' => false, 'error' => 'Failed to save configuration'];
        }

        // Regenerate service file
        $name = $config['output']['name'] ?? $id;
        generate_hls_service_file($id, $name, $config);
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
    // Check both 'output' and 'general' sections for type (for consistency)
    $type = $config['output']['type'] ?? $config['general']['type'] ?? 'srt';

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
        error_log("start_output_service: Config file not found: {$config_file}");
        return ['success' => false, 'error' => 'Output not found'];
    }

    $config = parse_config($config_file);
    // Check both 'output' and 'general' sections for type (for consistency)
    $type = $config['output']['type'] ?? $config['general']['type'] ?? 'srt';
    $service_name = $id . '-output-' . $type;

    error_log("start_output_service: id={$id}, type={$type}, service_name={$service_name}");

    // Use same endpoint as muxers
    $result = call_cari_api('/service/control', 'POST', [
        'action' => 'start',
        'service_name' => $service_name
    ]);

    error_log("start_output_service: API result: " . json_encode($result));

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
    // Check both 'output' and 'general' sections for type (for consistency)
    $type = $config['output']['type'] ?? $config['general']['type'] ?? 'srt';
    $service_name = $id . '-output-' . $type;

    error_log("stop_output_service: id={$id}, type={$type}, service_name={$service_name}");

    // Use same endpoint as muxers
    $result = call_cari_api('/service/control', 'POST', [
        'action' => 'stop',
        'service_name' => $service_name
    ]);

    error_log("stop_output_service: API result: " . json_encode($result));

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
    // Check both 'output' and 'general' sections for type (for consistency)
    $type = $config['output']['type'] ?? $config['general']['type'] ?? 'srt';
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
    // Check both 'output' and 'general' sections for type (for consistency)
    $type = $config['output']['type'] ?? $config['general']['type'] ?? 'srt';
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

    // Try root URL first (ristsender serves metrics at root), fallback to /metrics
    $url = "http://127.0.0.1:{$metrics_port}/";

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

    // If response is just "OK" or doesn't look like Prometheus metrics, try /metrics path
    if (strtolower(trim($response)) === 'ok' || strpos($response, '# HELP') === false) {
        $url_metrics = "http://127.0.0.1:{$metrics_port}/metrics";
        $response_metrics = @file_get_contents($url_metrics, false, $ctx);
        if ($response_metrics !== false && strpos($response_metrics, '# HELP') !== false) {
            $response = $response_metrics;
        }
    }

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

/**
 * Get HTTP MPEG-TS output stats from http_ts_server
 */
function get_http_stats($id) {
    $id = preg_replace('/[^a-zA-Z0-9_-]/', '', $id);
    $config_file = CONFIG_PATH . '/outputs/' . $id . '.conf';

    if (!file_exists($config_file)) {
        return ['success' => false, 'error' => 'Output not found'];
    }

    $config = parse_config($config_file);

    if (($config['output']['type'] ?? '') !== 'http') {
        return ['success' => false, 'error' => 'Not an HTTP output'];
    }

    $http_port = $config['destination_http']['listen_port'] ?? '8888';
    $stats_path = $config['destination_http']['stats_path'] ?? '/stats';

    $url = "http://127.0.0.1:{$http_port}{$stats_path}";

    $ctx = stream_context_create([
        'http' => [
            'method' => 'GET',
            'timeout' => 5,
            'ignore_errors' => true
        ]
    ]);

    $response = @file_get_contents($url, false, $ctx);

    if ($response === false) {
        return ['success' => false, 'error' => 'Cannot connect to HTTP stats server', 'status' => 'offline'];
    }

    $stats = json_decode($response, true);
    if (!$stats) {
        return ['success' => false, 'error' => 'Invalid stats response'];
    }

    return [
        'success' => true,
        'id' => $id,
        'name' => $config['output']['name'] ?? $id,
        'type' => 'http',
        'stats' => $stats
    ];
}

/**
 * Get HLS output stats from hls_output server
 */
function get_hls_stats($id) {
    $id = preg_replace('/[^a-zA-Z0-9_-]/', '', $id);
    $config_file = CONFIG_PATH . '/outputs/' . $id . '.conf';

    if (!file_exists($config_file)) {
        return ['success' => false, 'error' => 'Output not found'];
    }

    $config = parse_config($config_file);

    if (($config['output']['type'] ?? '') !== 'hls') {
        return ['success' => false, 'error' => 'Not an HLS output'];
    }

    $http_port = $config['destination_hls']['http_port'] ?? '8080';

    $url = "http://127.0.0.1:{$http_port}/stats";

    $ctx = stream_context_create([
        'http' => [
            'method' => 'GET',
            'timeout' => 5,
            'ignore_errors' => true
        ]
    ]);

    $response = @file_get_contents($url, false, $ctx);

    if ($response === false) {
        return ['success' => false, 'error' => 'Cannot connect to HLS stats server', 'status' => 'offline'];
    }

    $stats = json_decode($response, true);
    if (!$stats) {
        return ['success' => false, 'error' => 'Invalid stats response'];
    }

    return [
        'success' => true,
        'id' => $id,
        'name' => $config['output']['name'] ?? $id,
        'type' => 'hls',
        'stats' => $stats
    ];
}
