<?php
/**
 * CariTranscoder - Inputs API
 * Copyright (c) 2024 CariTech Solutions
 */

// Start output buffering to catch any unexpected output
ob_start();

// Set JSON content type early
header('Content-Type: application/json');

// Error handler to convert PHP errors to JSON
set_error_handler(function($errno, $errstr, $errfile, $errline) {
    // Respect @ error suppression operator
    if (error_reporting() === 0) {
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

// CariTranscoder API URL for privileged operations
define('CARI_API_URL', 'http://127.0.0.1:8081');

// Clear any output from includes
ob_end_clean();

// Require login
if (!auth_is_logged_in()) {
    json_response(['error' => 'Unauthorized'], 401);
}

$action = $_GET['action'] ?? $_POST['action'] ?? 'list';

switch ($action) {
    case 'list':
        $inputs = get_service_list('inputs');
        json_response(['inputs' => $inputs]);
        break;

    case 'get':
        $id = $_GET['id'] ?? '';
        if (empty($id)) {
            json_response(['error' => 'Input ID required'], 400);
        }
        $input = get_input_config($id);
        if ($input) {
            json_response($input);
        } else {
            json_response(['error' => 'Input not found'], 404);
        }
        break;

    case 'check_name':
        // Check if name is unique
        $name = $_GET['name'] ?? '';
        $exclude_id = $_GET['exclude'] ?? '';

        if (empty($name)) {
            json_response(['error' => 'Name required'], 400);
        }

        $id = sanitize_name_to_id($name);
        $exists = input_exists($id, $exclude_id);

        json_response([
            'available' => !$exists,
            'suggested_id' => $id,
            'buffer_name' => 'buffer-input-' . $id
        ]);
        break;

    case 'scan':
        // Scan source for PIDs
        $source = $_POST['source'] ?? $_GET['source'] ?? '';
        $type = $_POST['type'] ?? $_GET['type'] ?? 'udp';

        if (empty($source)) {
            json_response(['error' => 'Source URL required'], 400);
        }

        $pids = scan_source_pids($source, $type);
        json_response($pids);
        break;

    case 'create':
        // Create new input
        $data = $_POST;
        if (empty($data) || empty($data['name'])) {
            // Try JSON body
            $data = json_decode(file_get_contents('php://input'), true);
        }

        if (empty($data['name'])) {
            json_response(['error' => 'Input name required'], 400);
        }

        $result = create_input($data);
        json_response($result);
        break;

    case 'update':
        $id = $_GET['id'] ?? $_POST['id'] ?? '';
        if (empty($id)) {
            json_response(['error' => 'Input ID required'], 400);
        }

        $data = $_POST;
        if (empty($data) || count($data) <= 1) {
            $data = json_decode(file_get_contents('php://input'), true);
        }

        $result = update_input($id, $data);
        json_response($result);
        break;

    case 'delete':
        $id = $_GET['id'] ?? $_POST['id'] ?? '';
        if (empty($id)) {
            json_response(['error' => 'Input ID required'], 400);
        }

        $result = delete_input($id);
        json_response($result);
        break;

    case 'start':
        $id = $_GET['id'] ?? $_POST['id'] ?? '';
        if (empty($id)) {
            json_response(['error' => 'Input ID required'], 400);
        }

        $result = start_input_service($id);
        json_response($result);
        break;

    case 'stop':
        $id = $_GET['id'] ?? $_POST['id'] ?? '';
        if (empty($id)) {
            json_response(['error' => 'Input ID required'], 400);
        }

        $result = stop_input_service($id);
        json_response($result);
        break;

    case 'metrics':
        // Proxy metrics request to udp_input API
        $id = $_GET['id'] ?? '';
        if (empty($id)) {
            json_response(['error' => 'Input ID required'], 400);
        }

        $metrics = get_input_metrics($id);
        json_response($metrics);
        break;

    case 'all_metrics':
        // Get metrics for all running inputs
        $all_metrics = get_all_input_metrics();
        json_response($all_metrics);
        break;

    case 'metrics_history':
        // Get historical metrics for an input
        $id = $_GET['id'] ?? '';
        if (empty($id)) {
            json_response(['error' => 'Input ID required'], 400);
        }

        $history = get_input_metrics_history($id);
        json_response($history);
        break;

    case 'preview_start':
        // Start player_preview for an input
        $id = $_GET['id'] ?? '';
        if (empty($id)) {
            json_response(['error' => 'Input ID required'], 400);
        }

        $result = start_player_preview($id);
        json_response($result);
        break;

    case 'preview_status':
        // Get player_preview status
        $id = $_GET['id'] ?? '';
        if (empty($id)) {
            json_response(['error' => 'Input ID required'], 400);
        }

        $result = get_preview_status($id);
        json_response($result);
        break;

    case 'preview_keepalive':
        // Send keepalive to player_preview
        $id = $_GET['id'] ?? '';
        if (empty($id)) {
            json_response(['error' => 'Input ID required'], 400);
        }

        $result = send_preview_keepalive($id);
        json_response($result);
        break;

    default:
        json_response(['error' => 'Invalid action'], 400);
}

/**
 * Get metrics for a specific input from udp_input API
 */
function get_input_metrics($id) {
    $id = preg_replace('/[^a-zA-Z0-9_-]/', '', $id);
    $config_file = CONFIG_PATH . '/inputs/' . $id . '.conf';

    if (!file_exists($config_file)) {
        return ['success' => false, 'error' => 'Input not found'];
    }

    $config = parse_config($config_file);
    $api_port = $config['output']['api_port'] ?? null;

    if (!$api_port) {
        return ['success' => false, 'error' => 'No API port configured for this input'];
    }

    // Query the udp_input API
    $url = "http://127.0.0.1:{$api_port}/metrics";
    $ctx = stream_context_create([
        'http' => [
            'timeout' => 2,
            'ignore_errors' => true
        ]
    ]);

    $response = @file_get_contents($url, false, $ctx);

    if ($response === false) {
        return [
            'success' => false,
            'error' => 'Cannot connect to input monitor',
            'status' => 'offline'
        ];
    }

    $data = json_decode($response, true);
    if (!$data) {
        return ['success' => false, 'error' => 'Invalid response from input monitor'];
    }

    // Add input metadata
    $data['input_id'] = $id;
    $data['input_name'] = $config['general']['name'] ?? $id;
    $data['output_address'] = ($config['output']['address'] ?? '') . ':' . ($config['output']['port'] ?? '');
    $data['success'] = true;

    return $data;
}

/**
 * Get historical metrics for a specific input from udp_input API
 */
function get_input_metrics_history($id) {
    $id = preg_replace('/[^a-zA-Z0-9_-]/', '', $id);
    $config_file = CONFIG_PATH . '/inputs/' . $id . '.conf';

    if (!file_exists($config_file)) {
        return ['success' => false, 'error' => 'Input not found'];
    }

    $config = parse_config($config_file);
    $api_port = $config['output']['api_port'] ?? null;

    if (!$api_port) {
        return ['success' => false, 'error' => 'No API port configured for this input'];
    }

    // Query the udp_input API for history
    $url = "http://127.0.0.1:{$api_port}/metrics/history";
    $ctx = stream_context_create([
        'http' => [
            'timeout' => 10,  // History can be large, allow more time
            'ignore_errors' => true
        ]
    ]);

    $response = @file_get_contents($url, false, $ctx);

    if ($response === false) {
        return [
            'success' => false,
            'error' => 'Cannot connect to input monitor',
            'status' => 'offline'
        ];
    }

    $data = json_decode($response, true);
    if (!$data) {
        return ['success' => false, 'error' => 'Invalid response from input monitor'];
    }

    // Add input metadata
    $data['input_id'] = $id;
    $data['input_name'] = $config['general']['name'] ?? $id;
    $data['output_address'] = ($config['output']['address'] ?? '') . ':' . ($config['output']['port'] ?? '');
    $data['success'] = true;

    return $data;
}

/**
 * Get metrics for all inputs
 */
function get_all_input_metrics() {
    $inputs = get_service_list('inputs');
    $all_metrics = [];

    foreach ($inputs as $input) {
        $id = $input['id'];

        // Check if input is UDP type and has output config
        $config = $input['config'] ?? [];
        if (($config['general']['type'] ?? '') !== 'udp') {
            continue;
        }

        $api_port = $config['output']['api_port'] ?? null;
        if (!$api_port) {
            continue;
        }

        // Query the udp_input API
        $url = "http://127.0.0.1:{$api_port}/metrics";
        $ctx = stream_context_create([
            'http' => [
                'timeout' => 1,
                'ignore_errors' => true
            ]
        ]);

        $response = @file_get_contents($url, false, $ctx);

        if ($response === false) {
            $all_metrics[$id] = [
                'input_id' => $id,
                'input_name' => $input['name'],
                'status' => 'offline',
                'pids' => []
            ];
            continue;
        }

        $data = json_decode($response, true);
        if ($data) {
            $data['input_id'] = $id;
            $data['input_name'] = $input['name'];
            $data['output_address'] = ($config['output']['address'] ?? '') . ':' . ($config['output']['port'] ?? '');
            $all_metrics[$id] = $data;
        }
    }

    return ['success' => true, 'inputs' => $all_metrics];
}

/**
 * Get internal routing settings from main config
 */
function get_routing_config() {
    $main_config = parse_config(MAIN_CONFIG);
    return [
        'multicast_base' => $main_config['internal_routing']['multicast_base'] ?? '239.100.0.1',
        'multicast_port' => (int)($main_config['internal_routing']['multicast_port'] ?? 10000),
        'api_port_base' => (int)($main_config['internal_routing']['api_port_base'] ?? 9100)
    ];
}

/**
 * Get list of already allocated output addresses
 */
function get_allocated_addresses() {
    $allocated = [];
    $inputs_dir = CONFIG_PATH . '/inputs';

    if (!is_dir($inputs_dir)) {
        return $allocated;
    }

    foreach (glob($inputs_dir . '/*.conf') as $file) {
        $config = parse_config($file);
        if (isset($config['output']['address'])) {
            $allocated[] = [
                'address' => $config['output']['address'],
                'port' => $config['output']['port'] ?? 10000,
                'api_port' => $config['output']['api_port'] ?? 0
            ];
        }
    }

    return $allocated;
}

/**
 * Allocate next available output address and API port
 */
function allocate_output_address() {
    $routing = get_routing_config();
    $allocated = get_allocated_addresses();

    // Parse base address
    $base_parts = explode('.', $routing['multicast_base']);
    $base_int = ip2long($routing['multicast_base']);

    // Find next available address
    $used_addresses = array_column($allocated, 'address');
    $used_api_ports = array_column($allocated, 'api_port');

    $offset = 0;
    $address = null;
    $api_port = null;

    // Find next free address (max 254 inputs per /24)
    for ($i = 0; $i < 254; $i++) {
        $candidate = long2ip($base_int + $i);
        if (!in_array($candidate, $used_addresses)) {
            $address = $candidate;
            $offset = $i;
            break;
        }
    }

    // Find next free API port
    for ($i = 0; $i < 254; $i++) {
        $candidate = $routing['api_port_base'] + $i;
        if (!in_array($candidate, $used_api_ports)) {
            $api_port = $candidate;
            break;
        }
    }

    if (!$address || !$api_port) {
        return null; // Pool exhausted
    }

    return [
        'address' => $address,
        'port' => $routing['multicast_port'],
        'api_port' => $api_port
    ];
}

/**
 * Call the privileged CariTranscoder API
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

    $result = json_decode($response, true);
    if ($result === null) {
        return ['success' => false, 'error' => 'Invalid response from API'];
    }

    return $result;
}

/**
 * Generate systemd service file for udp_input via API
 */
function generate_udp_input_service($id, $config) {
    // Get source info
    $source_url = '';
    $source_type = $config['general']['type'] ?? 'udp';

    if (isset($config['sources'])) {
        foreach ($config['sources'] as $key => $value) {
            if (strpos($key, 'source_') === 0) {
                $parts = explode('|', $value);
                $source_type = $parts[0] ?? 'udp';
                $source_url = $parts[1] ?? '';
                break; // Use primary source
            }
        }
    }

    // Only generate for UDP inputs (for now)
    if ($source_type !== 'udp') {
        return ['success' => false, 'error' => 'Only UDP inputs supported currently'];
    }

    // Parse source URL (format: address:port or just address)
    $source_parts = explode(':', $source_url);
    $input_addr = $source_parts[0];
    $input_port = (int)($source_parts[1] ?? 5000);

    // Get output address
    $output_addr = $config['output']['address'] ?? '';
    $output_port = (int)($config['output']['port'] ?? 10000);
    $api_port = (int)($config['output']['api_port'] ?? 9100);

    if (!$output_addr) {
        return ['success' => false, 'error' => 'No output address configured'];
    }

    // Get PIDs
    $program_pid = $config['pids']['program'] ?? '';
    $video_pid = $config['pids']['video'] ?? '';
    $audio_pids = $config['pids']['audio'] ?? '';

    if (empty($program_pid)) {
        return ['success' => false, 'error' => 'Program PID not configured'];
    }

    // Build PIDs list for monitoring (video and audio)
    $monitor_pids = [];
    if ($video_pid) {
        $monitor_pids[] = $video_pid;
    }
    if ($audio_pids) {
        foreach (explode(',', $audio_pids) as $pid) {
            $pid = trim($pid);
            if ($pid) {
                $monitor_pids[] = $pid;
            }
        }
    }

    $name = $config['general']['name'] ?? $id;

    // Call the API to create the service
    $api_data = [
        'id' => $id,
        'source_address' => $input_addr,
        'source_port' => $input_port,
        'output_address' => $output_addr,
        'output_port' => $output_port,
        'api_port' => $api_port,
        'program' => (int)$program_pid,
        'description' => "CariTranscoder UDP Input - {$name}"
    ];

    // Add PIDs for monitoring if available
    if (!empty($monitor_pids)) {
        $api_data['pids'] = implode(',', $monitor_pids);
    }

    $result = call_cari_api('/input/udp/create', 'POST', $api_data);

    return $result;
}

/**
 * Sanitize name to ID (lowercase, alphanumeric, hyphens)
 */
function sanitize_name_to_id($name) {
    $id = strtolower(trim($name));
    $id = preg_replace('/[^a-z0-9]+/', '-', $id);
    $id = trim($id, '-');
    return $id ?: 'input-' . time();
}

/**
 * Check if input exists
 */
function input_exists($id, $exclude_id = '') {
    $config_file = CONFIG_DIR . '/inputs/' . $id . '.conf';
    if ($exclude_id && $id === $exclude_id) {
        return false;
    }
    return file_exists($config_file);
}

/**
 * Get input configuration
 */
function get_input_config($id) {
    $id = preg_replace('/[^a-zA-Z0-9_-]/', '', $id);
    $config_file = CONFIG_DIR . '/inputs/' . $id . '.conf';

    if (!file_exists($config_file)) {
        return null;
    }

    $config = parse_ini_file($config_file, true);
    $config['id'] = $id;

    // Parse sources array
    if (isset($config['sources'])) {
        $sources = [];
        foreach ($config['sources'] as $key => $value) {
            if (strpos($key, 'source_') === 0) {
                $idx = str_replace('source_', '', $key);
                $parts = explode('|', $value);
                $sources[] = [
                    'url' => $parts[0] ?? '',
                    'weight' => (int)($parts[1] ?? 10)
                ];
            }
        }
        $config['sources_list'] = $sources;
    }

    // Parse PIDs
    if (isset($config['pids'])) {
        $config['video_pid'] = $config['pids']['video'] ?? '';
        $config['audio_pids'] = isset($config['pids']['audio']) ? explode(',', $config['pids']['audio']) : [];
        $config['program_pid'] = $config['pids']['program'] ?? '';
    }

    return $config;
}

/**
 * Scan source for PIDs using TSDuck or ffprobe
 * TSDuck works best for: UDP, SRT, RIST, File
 * FFprobe works best for: RTMP, HLS, HTTP streams
 */
function scan_source_pids($source, $type = 'udp') {
    $result = [
        'success' => false,
        'programs' => [],
        'video_pids' => [],
        'audio_pids' => [],
        'error' => null
    ];

    // Build the source URL based on type
    $scan_url = build_source_url($source, $type);

    // Choose scanning method based on input type
    // TSDuck is preferred for transport stream protocols
    $tsduck_types = ['udp', 'srt', 'rist', 'file'];
    // FFprobe is preferred for HTTP-based and RTMP protocols
    $ffprobe_types = ['rtmp', 'hls', 'http', 'https'];

    $tsduck_available = shell_exec('which tsp 2>/dev/null');
    $ffprobe_available = shell_exec('which ffprobe 2>/dev/null');

    if (in_array($type, $tsduck_types) && $tsduck_available) {
        $result = scan_with_tsduck($scan_url, $type);
    } elseif ($ffprobe_available) {
        // Use ffprobe for HTTP-based protocols or as fallback
        $result = scan_with_ffprobe($scan_url, $type);
    } else {
        $result['error'] = 'No scanning tools available (install TSDuck or FFmpeg)';
    }

    // If TSDuck failed, try ffprobe as fallback
    if (!$result['success'] && $ffprobe_available && in_array($type, $tsduck_types)) {
        $ffprobe_result = scan_with_ffprobe($scan_url, $type);
        if ($ffprobe_result['success']) {
            $result = $ffprobe_result;
        }
    }

    return $result;
}

/**
 * Build source URL from parameters
 */
function build_source_url($source, $type) {
    // If source is already a full URL, return it
    if (preg_match('/^(udp|srt|rist|rtmp|http|https):\/\//', $source)) {
        return $source;
    }

    // Otherwise build based on type
    switch ($type) {
        case 'udp':
            // Assume format: address:port or just address (default port 5000)
            if (strpos($source, ':') === false) {
                $source .= ':5000';
            }
            return 'udp://' . $source;
        case 'srt':
            if (strpos($source, ':') === false) {
                $source .= ':9000';
            }
            return 'srt://' . $source;
        case 'rist':
            // RIST URL format: rist://address:port
            if (strpos($source, ':') === false) {
                $source .= ':5000';
            }
            return 'rist://' . $source;
        case 'file':
            // File path - return as-is
            return $source;
        case 'rtmp':
            // RTMP should already have full URL
            return $source;
        case 'hls':
            // HLS should already have full URL
            return $source;
        default:
            return $source;
    }
}

/**
 * Scan using TSDuck (capture + tsanalyze)
 */
function scan_with_tsduck($url, $type) {
    $result = [
        'success' => false,
        'programs' => [],
        'video_pids' => [],
        'audio_pids' => [],
        'error' => null
    ];

    $capture_file = '/tmp/caritrans_scan_' . uniqid() . '.ts';
    $capture_cmd = '';

    // Build capture command based on input type
    if ($type === 'udp' || strpos($url, 'udp://') === 0) {
        $parsed = parse_url(str_replace('udp://', 'http://', $url));
        $address = $parsed['host'] ?? '';
        $port = $parsed['port'] ?? 5000;

        $capture_cmd = sprintf(
            'timeout 5 tsp -I ip %s:%d -O file %s 2>&1',
            escapeshellarg($address),
            (int)$port,
            escapeshellarg($capture_file)
        );
    } elseif ($type === 'srt' || strpos($url, 'srt://') === 0) {
        // SRT input - prefer srt-live-transmit if available
        $srt_transmit = trim(shell_exec('which srt-live-transmit 2>/dev/null'));

        // Ensure URL has srt:// prefix
        $srt_url = $url;
        if (strpos($url, 'srt://') !== 0) {
            $srt_url = 'srt://' . $url;
        }

        if ($srt_transmit) {
            // Use srt-live-transmit to receive SRT and output to stdout, redirect to file
            // file://con outputs to stdout which we redirect to the capture file
            // stderr must go to /dev/null to avoid corrupting the TS capture
            $capture_cmd = sprintf(
                'timeout 8 srt-live-transmit %s file://con 2>/dev/null > %s',
                escapeshellarg($srt_url),
                escapeshellarg($capture_file)
            );
        } else {
            // Fallback to TSDuck SRT plugin
            $srt_addr = preg_replace('/^srt:\/\//', '', $url);
            $parts = explode(':', $srt_addr);
            $address = $parts[0] ?? '';
            $port = $parts[1] ?? 9000;

            $capture_cmd = sprintf(
                'timeout 8 tsp -I srt --caller %s:%d -O file %s 2>&1',
                escapeshellarg($address),
                (int)$port,
                escapeshellarg($capture_file)
            );
        }
    } elseif ($type === 'rist' || strpos($url, 'rist://') === 0) {
        // RIST input - prefer ristreceiver if available
        $rist_receiver = trim(shell_exec('which ristreceiver 2>/dev/null') ?:
                         (file_exists('/usr/local/bin/ristreceiver') ? '/usr/local/bin/ristreceiver' : ''));

        // Ensure URL has rist:// prefix
        $rist_url = $url;
        if (strpos($url, 'rist://') !== 0) {
            $rist_url = 'rist://' . $url;
        }

        if ($rist_receiver) {
            // Use ristreceiver to receive RIST and output to local multicast UDP
            // ristreceiver requires -i (input) and -o (output) parameters
            // We output to a multicast address and capture with tsp
            $local_port = rand(15000, 15999);
            $probe_multicast = "udp://239.7.7.7:{$local_port}";

            // Start ristreceiver in background with timeout (auto-terminates, no pkill needed)
            $rist_cmd = sprintf(
                'timeout 10 %s -i %s -o %s -S 0 -v -1 > /dev/null 2>&1 &',
                $rist_receiver,
                escapeshellarg($rist_url),
                escapeshellarg($probe_multicast)
            );
            exec($rist_cmd);

            // Give ristreceiver a moment to start
            usleep(500000); // 500ms

            // Capture from multicast with tsp (shorter timeout than ristreceiver)
            $capture_cmd = sprintf(
                'timeout 8 tsp -I ip 239.7.7.7:%d -O file %s 2>&1',
                $local_port,
                escapeshellarg($capture_file)
            );
        } else {
            // Fallback to TSDuck RIST plugin
            $capture_cmd = sprintf(
                'timeout 8 tsp -I rist %s -O file %s 2>&1',
                escapeshellarg($rist_url),
                escapeshellarg($capture_file)
            );
        }
    } elseif ($type === 'file') {
        // For file input, just analyze directly without capture
        if (!file_exists($url)) {
            $result['error'] = 'File not found: ' . $url;
            return $result;
        }
        // Analyze file directly
        $analyze_cmd = sprintf('tsanalyze %s 2>/dev/null | cat', escapeshellarg($url));
        exec($analyze_cmd, $output, $code);

        if (empty($output)) {
            $result['error'] = 'Failed to analyze file';
            return $result;
        }

        // Parse the output (shared with capture flow below)
        return parse_tsduck_output($output, $result);
    } else {
        $result['error'] = 'Unsupported input type for TSDuck scanning: ' . $type;
        return $result;
    }

    // Capture stream
    exec($capture_cmd, $capture_output, $capture_code);

    // Check if capture file was created
    if (!file_exists($capture_file) || filesize($capture_file) < 1000) {
        if (file_exists($capture_file)) {
            unlink($capture_file);
        }
        $error_msg = 'Failed to capture stream (no data received).';
        if (!empty($capture_output)) {
            $error_msg .= ' TSDuck output: ' . implode(' ', array_slice($capture_output, 0, 3));
        }
        $result['error'] = $error_msg;
        $result['debug'] = [
            'command' => $capture_cmd,
            'exit_code' => $capture_code,
            'output' => $capture_output
        ];
        return $result;
    }

    // Analyze captured file with tsanalyze
    $analyze_cmd = sprintf('tsanalyze %s 2>/dev/null | cat', escapeshellarg($capture_file));
    exec($analyze_cmd, $output, $code);

    // Clean up capture file
    if (file_exists($capture_file)) {
        unlink($capture_file);
    }

    if (empty($output)) {
        $result['error'] = 'Failed to analyze captured stream';
        return $result;
    }

    return parse_tsduck_output($output, $result);
}

/**
 * Parse TSDuck tsanalyze output
 */
function parse_tsduck_output($output, $result = null) {
    if ($result === null) {
        $result = [
            'success' => false,
            'programs' => [],
            'video_pids' => [],
            'audio_pids' => [],
            'error' => null
        ];
    }

    $output_text = is_array($output) ? implode("\n", $output) : $output;

    // Parse service info: "Service: 0x03E8 (1000)" and "Service name: BET"
    if (preg_match('/Service:\s*0x([0-9A-Fa-f]+)\s*\((\d+)\)/', $output_text, $svc_match)) {
        $program = [
            'id' => (int)$svc_match[2],
            'name' => 'Program ' . $svc_match[2]
        ];
        // Try to get service name
        if (preg_match('/Service name:\s*([^,\n]+)/i', $output_text, $name_match)) {
            $program['name'] = trim($name_match[1]);
        }
        $result['programs'][] = $program;
    }

    // Parse PIDs from lines like:
    // |  0x00D3  HEVC video (960x736, main profile, level 3.1,  C    1,391,690 b/s  |
    // |  0x00DD  MPEG-2 AAC Audio (eng, Audio layer 0, @16,000  C      134,240 b/s  |
    $lines = explode("\n", $output_text);
    foreach ($lines as $line) {
        // Match video PIDs - look for "video" in the line with hex PID
        if (preg_match('/\|\s*0x([0-9A-Fa-f]+)\s+(.+?video.+?)\s+C/i', $line, $match)) {
            $pid = hexdec($match[1]);
            $desc = trim($match[2]);

            // Extract codec (H.264, HEVC, MPEG-2, etc.)
            $codec = 'Video';
            if (stripos($desc, 'HEVC') !== false || stripos($desc, 'H.265') !== false) {
                $codec = 'HEVC';
            } elseif (stripos($desc, 'AVC') !== false || stripos($desc, 'H.264') !== false) {
                $codec = 'H.264';
            } elseif (stripos($desc, 'MPEG-2') !== false) {
                $codec = 'MPEG-2';
            }

            // Extract resolution if present
            $width = 0;
            $height = 0;
            if (preg_match('/(\d+)x(\d+)/', $desc, $res_match)) {
                $width = (int)$res_match[1];
                $height = (int)$res_match[2];
            }

            $result['video_pids'][] = [
                'pid' => $pid,
                'codec' => $codec,
                'width' => $width,
                'height' => $height,
                'description' => $desc
            ];
        }

        // Match audio PIDs - look for "Audio" in the line with hex PID
        if (preg_match('/\|\s*0x([0-9A-Fa-f]+)\s+(.+?Audio.+?)\s+C/i', $line, $match)) {
            $pid = hexdec($match[1]);
            $desc = trim($match[2]);

            // Extract codec
            $codec = 'Audio';
            if (stripos($desc, 'AAC') !== false) {
                $codec = 'AAC';
            } elseif (stripos($desc, 'AC-3') !== false || stripos($desc, 'AC3') !== false) {
                $codec = 'AC-3';
            } elseif (stripos($desc, 'E-AC-3') !== false || stripos($desc, 'EAC3') !== false) {
                $codec = 'E-AC-3';
            } elseif (stripos($desc, 'MPEG') !== false) {
                $codec = 'MPEG Audio';
            }

            // Extract language if present (e.g., "eng", "spa")
            $language = 'und';
            if (preg_match('/\(([a-z]{3}),/i', $desc, $lang_match)) {
                $language = strtolower($lang_match[1]);
            }

            $result['audio_pids'][] = [
                'pid' => $pid,
                'codec' => $codec,
                'language' => $language,
                'description' => $desc
            ];
        }
    }

    $result['success'] = !empty($result['video_pids']) || !empty($result['audio_pids']);
    if (!$result['success']) {
        $result['error'] = 'No video or audio PIDs found in stream';
        $result['raw_output'] = $output_text;
    }

    return $result;
}

/**
 * Scan using ffprobe (works well for RTMP, HLS, HTTP streams, and as fallback for others)
 */
function scan_with_ffprobe($url, $type = null) {
    $result = [
        'success' => false,
        'programs' => [],
        'video_pids' => [],
        'audio_pids' => [],
        'error' => null
    ];

    // Adjust timeout based on type - HLS may need longer
    $timeout = ($type === 'hls') ? 15 : 10;

    $cmd = sprintf(
        'timeout %d ffprobe -v quiet -show_programs -show_streams -print_format json %s 2>/dev/null',
        $timeout,
        escapeshellarg($url)
    );

    exec($cmd, $output, $code);

    if ($code !== 0 || empty($output)) {
        $result['error'] = 'Failed to scan source with ffprobe';
        return $result;
    }

    $json = json_decode(implode('', $output), true);

    if (!$json) {
        $result['error'] = 'Invalid response from ffprobe';
        return $result;
    }

    // Parse programs
    if (isset($json['programs'])) {
        foreach ($json['programs'] as $program) {
            $result['programs'][] = [
                'id' => $program['program_id'] ?? 0,
                'name' => $program['tags']['service_name'] ?? ('Program ' . ($program['program_id'] ?? 0))
            ];
        }
    }

    // Parse streams
    if (isset($json['streams'])) {
        foreach ($json['streams'] as $stream) {
            $pid = $stream['id'] ?? null;
            if ($pid && strpos($pid, '0x') === 0) {
                $pid = hexdec($pid);
            }

            if ($stream['codec_type'] === 'video') {
                $result['video_pids'][] = [
                    'pid' => (int)$pid,
                    'codec' => strtoupper($stream['codec_name'] ?? 'unknown'),
                    'width' => $stream['width'] ?? 0,
                    'height' => $stream['height'] ?? 0,
                    'description' => sprintf('%s %dx%d',
                        strtoupper($stream['codec_name'] ?? 'Video'),
                        $stream['width'] ?? 0,
                        $stream['height'] ?? 0
                    )
                ];
            } elseif ($stream['codec_type'] === 'audio') {
                $result['audio_pids'][] = [
                    'pid' => (int)$pid,
                    'codec' => strtoupper($stream['codec_name'] ?? 'unknown'),
                    'language' => $stream['tags']['language'] ?? 'und',
                    'channels' => $stream['channels'] ?? 2,
                    'sample_rate' => $stream['sample_rate'] ?? 48000,
                    'description' => sprintf('%s %s (%dch)',
                        strtoupper($stream['codec_name'] ?? 'Audio'),
                        $stream['tags']['language'] ?? 'und',
                        $stream['channels'] ?? 2
                    )
                ];
            }
        }
    }

    $result['success'] = !empty($result['video_pids']) || !empty($result['audio_pids']);

    return $result;
}

/**
 * Create a new input
 */
function create_input($data) {
    $name = trim($data['name'] ?? '');
    if (empty($name)) {
        return ['success' => false, 'error' => 'Input name required'];
    }

    $id = sanitize_name_to_id($name);

    // Check if already exists
    if (input_exists($id)) {
        return ['success' => false, 'error' => 'An input with this name already exists'];
    }

    // Ensure config directory exists
    $config_dir = CONFIG_DIR . '/inputs';
    if (!is_dir($config_dir)) {
        mkdir($config_dir, 0755, true);
    }

    // Build config
    // Get primary type from first source
    $primaryType = 'udp';
    if (!empty($data['sources']) && isset($data['sources'][0]['type'])) {
        $primaryType = $data['sources'][0]['type'];
    }

    // Get PIDs from primary source for backwards compatibility
    $primaryVideo = '';
    $primaryAudio = '';
    $primaryProgram = '';

    $sources = $data['sources'] ?? [];
    if (!empty($sources) && isset($sources[0])) {
        $primaryVideo = $sources[0]['video_pid'] ?? '';
        $primaryAudio = is_array($sources[0]['audio_pids'] ?? null) ? implode(',', $sources[0]['audio_pids']) : ($sources[0]['audio_pids'] ?? '');
        $primaryProgram = $sources[0]['program_pid'] ?? '';
    }

    // Fall back to global PIDs if per-source not set
    if (empty($primaryVideo)) $primaryVideo = $data['video_pid'] ?? '';
    if (empty($primaryAudio)) $primaryAudio = is_array($data['audio_pids'] ?? null) ? implode(',', $data['audio_pids']) : ($data['audio_pids'] ?? '');
    if (empty($primaryProgram)) $primaryProgram = $data['program_pid'] ?? '';

    // Allocate output address and API port for UDP inputs
    $output_alloc = null;
    if ($primaryType === 'udp') {
        $output_alloc = allocate_output_address();
        if (!$output_alloc) {
            return ['success' => false, 'error' => 'No available output addresses in pool'];
        }
    }

    $config = [
        'general' => [
            'name' => $name,
            'type' => $primaryType,  // Primary type for display
            'enabled' => 1,
            'buffer' => $data['buffer'] ?? 'buffer-input-' . $id
        ],
        'sources' => [],
        'pids' => [
            'video' => $primaryVideo,
            'audio' => $primaryAudio,
            'program' => $primaryProgram
        ],
        'output' => $output_alloc ? [
            'address' => $output_alloc['address'],
            'port' => $output_alloc['port'],
            'api_port' => $output_alloc['api_port']
        ] : []
    ];

    // Add sources with per-source type, settings, and PIDs
    if (!empty($sources)) {
        foreach ($sources as $idx => $source) {
            $url = $source['url'] ?? $source;
            $type = $source['type'] ?? 'udp';
            $weight = $source['weight'] ?? 10;

            // Build source string: type|url|weight|extra_settings
            $sourceStr = "{$type}|{$url}|{$weight}";

            // Add type-specific settings
            $extraSettings = [];
            if ($type === 'srt') {
                $mode = $source['srt_mode'] ?? 'caller';
                $latency = $source['srt_latency'] ?? 200;
                $passphrase = $source['srt_passphrase'] ?? '';
                $extraSettings[] = "mode={$mode}";
                $extraSettings[] = "latency={$latency}";
                if ($passphrase) {
                    $extraSettings[] = "passphrase={$passphrase}";
                }
            } elseif ($type === 'rist') {
                $profile = $source['rist_profile'] ?? 'main';
                $buffer = $source['rist_buffer'] ?? 1000;
                $secret = $source['rist_secret'] ?? '';
                $extraSettings[] = "profile={$profile}";
                $extraSettings[] = "buffer={$buffer}";
                if ($secret) {
                    $extraSettings[] = "secret={$secret}";
                }
            } elseif ($type === 'file') {
                $loop = $source['file_loop'] ?? '1';
                $extraSettings[] = "loop={$loop}";
            }

            // Add per-source PIDs
            if (!empty($source['video_pid'])) {
                $extraSettings[] = "video_pid={$source['video_pid']}";
            }
            if (!empty($source['audio_pids'])) {
                $audioPids = is_array($source['audio_pids']) ? implode(';', $source['audio_pids']) : $source['audio_pids'];
                $extraSettings[] = "audio_pids={$audioPids}";
            }
            if (!empty($source['program_pid'])) {
                $extraSettings[] = "program_pid={$source['program_pid']}";
            }

            if (!empty($extraSettings)) {
                $sourceStr .= '|' . implode(',', $extraSettings);
            }

            $config['sources']['source_' . $idx] = $sourceStr;
        }
    }

    // Legacy type-specific settings (for backwards compatibility)
    switch ($primaryType) {
        case 'udp':
            // UDP settings extracted from first source URL
            break;
        case 'srt':
            // SRT settings stored per-source
            break;
        case 'rtmp':
            // RTMP settings stored in URL
            break;
        case 'hls':
            // HLS settings stored in URL
            break;
        case 'file':
            $config['file'] = [
                'path' => $data['file_path'] ?? '',
                'loop' => $data['file_loop'] ?? 1
            ];
            break;
    }

    // Write config file
    $config_file = $config_dir . '/' . $id . '.conf';
    $content = build_ini_content($config);

    // Check if directory is writable
    if (!is_writable($config_dir)) {
        return ['success' => false, 'error' => "Config directory not writable: $config_dir (check permissions)"];
    }

    $result = file_put_contents($config_file, $content);
    if ($result !== false) {
        // Set permissions to 664 so both owner and group (www-data) can read/write
        chmod($config_file, 0664);

        // Generate systemd service for UDP inputs
        $service_result = null;
        if ($primaryType === 'udp') {
            $service_result = generate_udp_input_service($id, $config);
        }

        $response = [
            'success' => true,
            'id' => $id,
            'message' => "Input '{$name}' created successfully"
        ];

        if ($output_alloc) {
            $response['output'] = $output_alloc;
        }

        if ($service_result) {
            if (!$service_result['success']) {
                $response['warning'] = 'Config saved but systemd service generation failed: ' . ($service_result['error'] ?? 'unknown');
            } else {
                $response['service_file'] = $service_result['service_file'] ?? null;
            }
        }

        return $response;
    }

    // Get more detailed error
    $error = error_get_last();
    $errorMsg = $error ? $error['message'] : 'Unknown error';
    return ['success' => false, 'error' => "Failed to save configuration: $errorMsg"];
}

/**
 * Update an existing input
 */
function update_input($id, $data) {
    $id = preg_replace('/[^a-zA-Z0-9_-]/', '', $id);
    $config_file = CONFIG_DIR . '/inputs/' . $id . '.conf';

    if (!file_exists($config_file)) {
        return ['success' => false, 'error' => 'Input not found'];
    }

    // Check if file is writable
    if (!is_writable($config_file)) {
        return ['success' => false, 'error' => 'Config file not writable (check permissions)'];
    }

    // Load existing config using our custom parser
    $config = parse_config($config_file);

    // Ensure sections exist
    if (!isset($config['general'])) $config['general'] = [];
    if (!isset($config['sources'])) $config['sources'] = [];
    if (!isset($config['pids'])) $config['pids'] = [];

    // Update general fields
    if (isset($data['name'])) {
        $config['general']['name'] = $data['name'];
    }
    if (isset($data['type'])) {
        $config['general']['type'] = $data['type'];
    }
    if (isset($data['enabled'])) {
        $config['general']['enabled'] = $data['enabled'] ? 1 : 0;
    }
    if (isset($data['buffer'])) {
        $config['general']['buffer'] = $data['buffer'];
    }

    // Update PIDs
    if (isset($data['video_pid'])) {
        $config['pids']['video'] = $data['video_pid'];
    }
    if (isset($data['audio_pids'])) {
        $config['pids']['audio'] = is_array($data['audio_pids']) ? implode(',', $data['audio_pids']) : $data['audio_pids'];
    }
    if (isset($data['program_pid'])) {
        $config['pids']['program'] = $data['program_pid'];
    }

    // Update sources (with proper type|url|weight|settings format including PIDs)
    if (isset($data['sources']) && is_array($data['sources'])) {
        $config['sources'] = [];
        $primaryType = 'udp';
        foreach ($data['sources'] as $idx => $source) {
            $type = $source['type'] ?? 'udp';
            $url = $source['url'] ?? $source;
            $weight = $source['weight'] ?? 10;

            // Store first source type as primary type
            if ($idx == 0) {
                $primaryType = $type;

                // Also update global PIDs from primary source for backwards compatibility
                if (!empty($source['video_pid'])) {
                    $config['pids']['video'] = $source['video_pid'];
                }
                if (!empty($source['audio_pids'])) {
                    $config['pids']['audio'] = is_array($source['audio_pids']) ? implode(',', $source['audio_pids']) : $source['audio_pids'];
                }
                if (!empty($source['program_pid'])) {
                    $config['pids']['program'] = $source['program_pid'];
                }
            }

            // Build source string: type|url|weight|extra_settings
            $sourceStr = "{$type}|{$url}|{$weight}";

            // Add type-specific settings and per-source PIDs
            $extraSettings = [];
            if ($type === 'srt') {
                $mode = $source['srt_mode'] ?? 'caller';
                $latency = $source['srt_latency'] ?? 200;
                $passphrase = $source['srt_passphrase'] ?? '';
                $extraSettings[] = "mode={$mode}";
                $extraSettings[] = "latency={$latency}";
                if ($passphrase) {
                    $extraSettings[] = "passphrase={$passphrase}";
                }
            } elseif ($type === 'rist') {
                $profile = $source['rist_profile'] ?? 'main';
                $buffer = $source['rist_buffer'] ?? 1000;
                $secret = $source['rist_secret'] ?? '';
                $extraSettings[] = "profile={$profile}";
                $extraSettings[] = "buffer={$buffer}";
                if ($secret) {
                    $extraSettings[] = "secret={$secret}";
                }
            } elseif ($type === 'file') {
                $loop = $source['file_loop'] ?? '1';
                $extraSettings[] = "loop={$loop}";
            }

            // Add per-source PIDs
            if (!empty($source['video_pid'])) {
                $extraSettings[] = "video_pid={$source['video_pid']}";
            }
            if (!empty($source['audio_pids'])) {
                $audioPids = is_array($source['audio_pids']) ? implode(';', $source['audio_pids']) : $source['audio_pids'];
                $extraSettings[] = "audio_pids={$audioPids}";
            }
            if (!empty($source['program_pid'])) {
                $extraSettings[] = "program_pid={$source['program_pid']}";
            }

            if (!empty($extraSettings)) {
                $sourceStr .= '|' . implode(',', $extraSettings);
            }

            $config['sources']['source_' . $idx] = $sourceStr;
        }
        // Update primary type
        $config['general']['type'] = $primaryType;
    }

    // Write updated config
    $content = build_ini_content($config);

    $result = file_put_contents($config_file, $content);
    if ($result !== false) {
        $response = ['success' => true, 'message' => "Input updated successfully"];

        // Regenerate systemd service for UDP inputs
        $type = $config['general']['type'] ?? 'udp';
        if ($type === 'udp') {
            $service_result = generate_udp_input_service($id, $config);
            if ($service_result && !$service_result['success']) {
                $response['warning'] = 'Config saved but systemd service generation failed: ' . ($service_result['error'] ?? 'unknown');
            }
        }

        return $response;
    }

    // Get detailed error
    $error = error_get_last();
    $errorMsg = $error ? $error['message'] : 'Unknown error';
    return ['success' => false, 'error' => "Failed to save configuration: $errorMsg"];
}

/**
 * Delete an input
 */
function delete_input($id) {
    $id = preg_replace('/[^a-zA-Z0-9_-]/', '', $id);

    if (empty($id)) {
        return ['success' => false, 'error' => 'Invalid input ID'];
    }

    // Get config to check type before deleting
    $config_file = CONFIG_DIR . '/inputs/' . $id . '.conf';
    $config = file_exists($config_file) ? parse_config($config_file) : [];
    $type = $config['general']['type'] ?? 'udp';

    // Stop and delete the systemd service via API
    if ($type === 'udp') {
        // Delete UDP input service (stops and removes service file)
        call_cari_api("/input/udp/{$id}", 'DELETE');
    } else {
        // Stop other service types
        stop_input_service($id);
    }

    // Remove config file
    if (!file_exists($config_file)) {
        return ['success' => false, 'error' => 'Input config file not found'];
    }

    // Check if directory is writable (needed for unlink)
    $config_dir = dirname($config_file);
    if (!is_writable($config_dir)) {
        return ['success' => false, 'error' => 'Config directory not writable'];
    }

    if (@unlink($config_file)) {
        return ['success' => true, 'message' => "Input deleted successfully"];
    }

    $error = error_get_last();
    $errorMsg = $error ? $error['message'] : 'Unknown error';
    return ['success' => false, 'error' => "Failed to delete: $errorMsg"];
}

/**
 * Start input service via API
 */
function start_input_service($id) {
    $id = preg_replace('/[^a-zA-Z0-9_-]/', '', $id);

    // Check input type to determine which service to use
    $config_file = CONFIG_DIR . '/inputs/' . $id . '.conf';
    $config = file_exists($config_file) ? parse_config($config_file) : [];
    $type = $config['general']['type'] ?? 'udp';

    // UDP inputs use the API endpoint directly
    if ($type === 'udp') {
        $result = call_cari_api("/input/udp/{$id}/start", 'POST');
        if (isset($result['success']) && $result['success']) {
            return ['success' => true, 'message' => "Input service started"];
        }
        return ['success' => false, 'error' => $result['error'] ?? $result['stderr'] ?? 'Failed to start service'];
    }

    // Other types use generic service control
    $service = "cari-input@{$id}";
    $result = call_cari_api('/service/control', 'POST', [
        'action' => 'start',
        'service_name' => $service
    ]);

    if (isset($result['success']) && $result['success']) {
        return ['success' => true, 'message' => "Input service started"];
    }

    return ['success' => false, 'error' => $result['error'] ?? $result['stderr'] ?? 'Failed to start service'];
}

/**
 * Stop input service via API
 */
function stop_input_service($id) {
    $id = preg_replace('/[^a-zA-Z0-9_-]/', '', $id);

    // Check input type to determine which service to use
    $config_file = CONFIG_DIR . '/inputs/' . $id . '.conf';
    $config = file_exists($config_file) ? parse_config($config_file) : [];
    $type = $config['general']['type'] ?? 'udp';

    // UDP inputs use the API endpoint directly
    if ($type === 'udp') {
        $result = call_cari_api("/input/udp/{$id}/stop", 'POST');
        if (isset($result['success']) && $result['success']) {
            return ['success' => true, 'message' => "Input service stopped"];
        }
        return ['success' => false, 'error' => $result['error'] ?? $result['stderr'] ?? 'Failed to stop service'];
    }

    // Other types use generic service control
    $service = "cari-input@{$id}";
    $result = call_cari_api('/service/control', 'POST', [
        'action' => 'stop',
        'service_name' => $service
    ]);

    if (isset($result['success']) && $result['success']) {
        return ['success' => true, 'message' => "Input service stopped"];
    }

    return ['success' => false, 'error' => $result['error'] ?? $result['stderr'] ?? 'Failed to stop service'];
}

/**
 * Build INI file content from array
 */
function build_ini_content($config) {
    $content = "; CariTranscoder Input Configuration\n";
    $content .= "; Generated: " . date('Y-m-d H:i:s') . "\n\n";

    foreach ($config as $section => $values) {
        $content .= "[{$section}]\n";
        foreach ($values as $key => $val) {
            if (is_array($val)) {
                $val = implode(',', $val);
            }
            $content .= "{$key} = {$val}\n";
        }
        $content .= "\n";
    }

    return $content;
}

/**
 * Get preview API port for an input (input api_port + 1000)
 */
function get_preview_port($id) {
    $config_file = CONFIG_PATH . '/inputs/' . $id . '.conf';
    if (!file_exists($config_file)) {
        return null;
    }
    $config = parse_config($config_file);
    $api_port = $config['output']['api_port'] ?? null;
    return $api_port ? (int)$api_port + 1000 : null;
}

/**
 * Get preview info for an input
 */
function get_preview_info($id) {
    $config_file = CONFIG_PATH . '/inputs/' . $id . '.conf';
    if (!file_exists($config_file)) {
        return null;
    }
    $config = parse_config($config_file);

    $name = $config['general']['name'] ?? $id;
    $folder_name = preg_replace('/[^a-zA-Z0-9_-]/', '-', strtolower($name));
    $output_addr = $config['output']['address'] ?? '';
    $output_port = $config['output']['port'] ?? '';
    $api_port = $config['output']['api_port'] ?? null;
    $preview_port = $api_port ? (int)$api_port + 1000 : null;

    // Use the web root directory (parent of 'api' folder)
    $web_root = dirname(__DIR__);

    return [
        'id' => $id,
        'name' => $name,
        'folder' => $folder_name,
        'input_address' => $output_addr . ':' . $output_port,
        'preview_port' => $preview_port,
        'output_dir' => $web_root . '/preview/' . $folder_name,
        'playlist_url' => '/preview/' . $folder_name . '/playlist.m3u8'
    ];
}

/**
 * Check if player_preview is running for an input
 */
function is_preview_running($id) {
    $preview_port = get_preview_port($id);
    if (!$preview_port) {
        return false;
    }

    // Try to connect to the preview API
    $ctx = stream_context_create([
        'http' => [
            'timeout' => 1,
            'ignore_errors' => true
        ]
    ]);

    $response = @file_get_contents("http://127.0.0.1:{$preview_port}/health", false, $ctx);
    return $response !== false;
}

/**
 * Start player_preview for an input
 */
function start_player_preview($id) {
    try {
        $id = preg_replace('/[^a-zA-Z0-9_-]/', '', $id);

        $info = get_preview_info($id);
        if (!$info) {
            return ['success' => false, 'error' => 'Input not found'];
        }

        // Validate we have required info
        if (empty($info['preview_port'])) {
            return ['success' => false, 'error' => 'No API port configured for this input'];
        }

        if (empty($info['input_address']) || $info['input_address'] === ':') {
            return ['success' => false, 'error' => 'No output address configured for this input'];
        }

        // Check if already running
        if (is_preview_running($id)) {
            // Send keepalive to extend timeout
            send_preview_keepalive($id);
            return [
                'success' => true,
                'already_running' => true,
                'message' => 'Preview already running',
                'playlist_url' => $info['playlist_url'],
                'preview_port' => $info['preview_port']
            ];
        }

        // Create output directory
        $output_dir = $info['output_dir'];
        if (!is_dir($output_dir)) {
            if (!@mkdir($output_dir, 0755, true)) {
                return ['success' => false, 'error' => 'Failed to create output directory: ' . $output_dir];
            }
        }

        // Call the CariTranscoder API to start player_preview
        // The API runs as root and can properly manage background processes
        $api_data = [
            'input_address' => $info['input_address'],
            'output_dir' => $output_dir,
            'api_port' => (int)$info['preview_port'],
            'folder' => $info['folder']
        ];

        $result = call_cari_api('/preview/start', 'POST', $api_data);

        if (isset($result['success']) && $result['success']) {
            return [
                'success' => true,
                'message' => $result['message'] ?? 'Preview started',
                'playlist_url' => $info['playlist_url'],
                'preview_port' => $info['preview_port'],
                'already_running' => $result['already_running'] ?? false
            ];
        }

        return [
            'success' => false,
            'error' => $result['error'] ?? 'Failed to start preview',
            'debug' => $result
        ];
    } catch (Exception $e) {
        return ['success' => false, 'error' => 'Exception: ' . $e->getMessage()];
    }
}

/**
 * Get player_preview status for an input
 */
function get_preview_status($id) {
    $id = preg_replace('/[^a-zA-Z0-9_-]/', '', $id);

    $info = get_preview_info($id);
    if (!$info) {
        return ['success' => false, 'error' => 'Input not found'];
    }

    $preview_port = $info['preview_port'];
    if (!$preview_port) {
        return ['success' => false, 'error' => 'No preview port configured'];
    }

    // Query player_preview status
    $ctx = stream_context_create([
        'http' => [
            'timeout' => 2,
            'ignore_errors' => true
        ]
    ]);

    $response = @file_get_contents("http://127.0.0.1:{$preview_port}/status", false, $ctx);

    if ($response === false) {
        return [
            'success' => true,
            'running' => false,
            'ready' => false,
            'playlist_url' => $info['playlist_url']
        ];
    }

    $data = json_decode($response, true);
    if (!$data) {
        return ['success' => false, 'error' => 'Invalid response from preview'];
    }

    return [
        'success' => true,
        'running' => true,
        'ready' => $data['ready'] ?? false,
        'segments' => $data['segments'] ?? 0,
        'ttl' => $data['ttl'] ?? 0,
        'playlist_url' => $info['playlist_url']
    ];
}

/**
 * Send keepalive to player_preview
 */
function send_preview_keepalive($id) {
    $id = preg_replace('/[^a-zA-Z0-9_-]/', '', $id);

    $preview_port = get_preview_port($id);
    if (!$preview_port) {
        return ['success' => false, 'error' => 'No preview port configured'];
    }

    // Send POST to keepalive endpoint
    $ctx = stream_context_create([
        'http' => [
            'method' => 'POST',
            'timeout' => 2,
            'ignore_errors' => true
        ]
    ]);

    $response = @file_get_contents("http://127.0.0.1:{$preview_port}/keepalive", false, $ctx);

    if ($response === false) {
        return ['success' => false, 'error' => 'Preview not running'];
    }

    $data = json_decode($response, true);
    return [
        'success' => true,
        'timeout' => $data['timeout'] ?? 60
    ];
}
