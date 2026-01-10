<?php
/**
 * CariTranscoder - Muxers API
 * TSDuck-based MPTS/SPTS multiplexing
 * Copyright (c) 2024 CariTech Solutions
 */

define('CARITRANS', true);
require_once __DIR__ . '/../../includes/config.php';
require_once __DIR__ . '/../../includes/auth.php';
require_once __DIR__ . '/../../includes/functions.php';

// CariTranscoder API URL (Python FastAPI running as root for systemd control)
define('CARI_API_URL', 'http://127.0.0.1:8081');

// Check authentication
if (!auth_is_logged_in()) {
    http_response_code(401);
    echo json_encode(['success' => false, 'error' => 'Unauthorized']);
    exit;
}

header('Content-Type: application/json');

$action = $_GET['action'] ?? '';
$id = $_GET['id'] ?? '';

// Route to appropriate handler
switch ($action) {
    case 'create':
        handle_create();
        break;
    case 'update':
        handle_update();
        break;
    case 'delete':
        handle_delete($id);
        break;
    case 'start':
        handle_start($id);
        break;
    case 'stop':
        handle_stop($id);
        break;
    case 'status':
        handle_status($id);
        break;
    case 'get':
        handle_get($id);
        break;
    case 'sources':
        handle_get_sources();
        break;
    case 'next_id':
        handle_get_next_id();
        break;
    case 'metrics':
        handle_metrics($id);
        break;
    case 'pid_stats':
        handle_pid_stats($id);
        break;
    case 'list':
    default:
        handle_list();
        break;
}

/**
 * DVB Service Types
 */
function get_service_types() {
    return [
        0x01 => 'Digital TV',
        0x02 => 'Digital Radio',
        0x0C => 'Data Broadcast',
        0x11 => 'MPEG-2 HD TV',
        0x16 => 'H.264/AVC SD TV',
        0x19 => 'H.264/AVC HD TV',
        0x1F => 'HEVC TV',
        0x20 => 'HEVC UHD TV'
    ];
}

/**
 * Call CariTranscoder API (Python FastAPI running as root for systemd control)
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
    return $result ?: ['success' => false, 'error' => 'Invalid API response'];
}

/**
 * Get available sources (inputs + transcoders) for mux input
 */
function handle_get_sources() {
    try {
        $sources = [];

        // Get inputs
        $inputs = get_service_list('inputs');
        foreach ($inputs as $input) {
            $config_file = CONFIG_PATH . '/inputs/' . $input['id'] . '.conf';
            $output_address = '';
            $output_port = '';

            if (file_exists($config_file)) {
                $config = parse_config($config_file);
                $output_address = $config['output']['address'] ?? '';
                $output_port = $config['output']['port'] ?? '';
            }

            if (!empty($output_address) && !empty($output_port)) {
                $sources[] = [
                    'type' => 'input',
                    'id' => $input['id'],
                    'name' => $input['name'],
                    'address' => $output_address,
                    'port' => $output_port,
                    'label' => "Input: {$input['name']} ({$output_address}:{$output_port})"
                ];
            }
        }

        // Get transcoders
        $transcoders = get_service_list('transcoders');
        foreach ($transcoders as $transcoder) {
            $config_file = CONFIG_PATH . '/transcoders/' . $transcoder['id'] . '.conf';
            $output_address = '';
            $output_port = '';

            if (file_exists($config_file)) {
                $config = parse_config($config_file);
                $output_address = $config['output']['address'] ?? '';
                $output_port = $config['output']['port'] ?? '';
            }

            if (!empty($output_address) && !empty($output_port)) {
                $sources[] = [
                    'type' => 'transcoder',
                    'id' => $transcoder['id'],
                    'name' => $transcoder['name'],
                    'address' => $output_address,
                    'port' => $output_port,
                    'label' => "Transcoder: {$transcoder['name']} ({$output_address}:{$output_port})"
                ];
            }
        }

        echo json_encode([
            'success' => true,
            'sources' => $sources,
            'service_types' => get_service_types()
        ]);
    } catch (Exception $e) {
        echo json_encode(['success' => false, 'error' => $e->getMessage()]);
    }
}

/**
 * Get next available muxer ID
 */
function handle_get_next_id() {
    $muxers_dir = CONFIG_PATH . '/muxers';
    $max_num = 0;

    if (is_dir($muxers_dir)) {
        $files = glob($muxers_dir . '/mux-*.conf');
        foreach ($files as $file) {
            $filename = basename($file, '.conf');
            if (preg_match('/^mux-(\d+)$/', $filename, $matches)) {
                $num = intval($matches[1]);
                if ($num > $max_num) {
                    $max_num = $num;
                }
            }
        }
    }

    $next_num = $max_num + 1;
    $next_id = 'mux-' . $next_num;
    $next_name = 'mux-' . $next_num;

    echo json_encode([
        'success' => true,
        'next_number' => $next_num,
        'next_id' => $next_id,
        'next_name' => $next_name
    ]);
}

/**
 * List all muxers with status
 */
function handle_list() {
    $muxers = get_service_list('muxers');

    // Enhance with status and additional info
    foreach ($muxers as &$mux) {
        $config_file = CONFIG_PATH . '/muxers/' . $mux['id'] . '.conf';
        if (file_exists($config_file)) {
            $config = parse_config($config_file);
            $mux['mode'] = $config['muxer']['mode'] ?? 'mpts';
            $mux['output_bitrate'] = intval($config['output']['output_bitrate'] ?? 0);
            $mux['output_address'] = $config['output']['address'] ?? '';
            $mux['output_port'] = $config['output']['port'] ?? '';

            // Count services
            $service_count = 0;
            if (isset($config['services'])) {
                foreach ($config['services'] as $key => $value) {
                    if (preg_match('/^service\.\d+\.enabled$/', $key) && $value === 'true') {
                        $service_count++;
                    }
                }
            }
            // Alternative: count service.X.enabled entries
            for ($i = 1; $i <= 20; $i++) {
                if (isset($config['services']["service.{$i}.enabled"]) &&
                    $config['services']["service.{$i}.enabled"] === 'true') {
                    $service_count++;
                }
            }
            $mux['service_count'] = $service_count;
        }

        // Get running status
        $mux['status'] = get_mux_status($mux['id']);
    }

    echo json_encode(['success' => true, 'muxers' => $muxers]);
}

/**
 * Get muxer status (running/stopped) via systemd
 */
function get_mux_status($id) {
    $service_name = "cari-mux@{$id}.service";
    exec("systemctl is-active " . escapeshellarg($service_name) . " 2>/dev/null", $output, $ret);

    if ($ret === 0 && !empty($output) && trim($output[0]) === 'active') {
        return 'running';
    }

    return 'stopped';
}

/**
 * Get single muxer config
 */
function handle_get($id) {
    if (empty($id)) {
        echo json_encode(['success' => false, 'error' => 'ID required']);
        return;
    }

    $id = preg_replace('/[^a-zA-Z0-9_-]/', '', $id);
    $config_file = CONFIG_PATH . '/muxers/' . $id . '.conf';

    if (!file_exists($config_file)) {
        echo json_encode(['success' => false, 'error' => 'Muxer not found']);
        return;
    }

    $config = parse_config($config_file);

    // Parse services into array format for UI
    $services = [];
    for ($i = 1; $i <= 20; $i++) {
        $prefix = "service.{$i}";
        if (isset($config['services']["{$prefix}.enabled"])) {
            $video_pid = intval($config['services']["{$prefix}.video_pid"] ?? (100 + ($i - 1) * 100));
            $audio_pid = intval($config['services']["{$prefix}.audio_pid"] ?? (101 + ($i - 1) * 100));
            $pcr_pid_val = intval($config['services']["{$prefix}.pcr_pid"] ?? $video_pid);

            $services[] = [
                'enabled' => $config['services']["{$prefix}.enabled"] === 'true',
                'source_type' => $config['services']["{$prefix}.source_type"] ?? 'input',
                'source_id' => $config['services']["{$prefix}.source_id"] ?? '',
                'source_address' => $config['services']["{$prefix}.source_address"] ?? '',
                'source_port' => $config['services']["{$prefix}.source_port"] ?? '',
                'program_number' => intval($config['services']["{$prefix}.program_number"] ?? ($i * 1000 + 1)),
                'service_name' => $config['services']["{$prefix}.service_name"] ?? '',
                'service_provider' => $config['services']["{$prefix}.service_provider"] ?? '',
                'service_type' => intval($config['services']["{$prefix}.service_type"] ?? 0x01),
                'pmt_pid' => intval($config['services']["{$prefix}.pmt_pid"] ?? (256 + ($i - 1))),
                'video_pid' => $video_pid,
                'audio_pid' => $audio_pid,
                'pcr_pid' => ($pcr_pid_val === $audio_pid) ? 'audio' : 'video',
                'is_pcr_reference' => ($config['services']["{$prefix}.is_pcr_reference"] ?? 'false') === 'true'
            ];
        }
    }

    echo json_encode([
        'success' => true,
        'config' => $config,
        'services' => $services,
        'service_types' => get_service_types()
    ]);
}

/**
 * Create a new muxer
 */
function handle_create() {
    $input = json_decode(file_get_contents('php://input'), true);
    if (!$input) {
        echo json_encode(['success' => false, 'error' => 'Invalid JSON input']);
        return;
    }

    // Validate required fields
    $required = ['name', 'output_bitrate', 'output_address', 'output_port'];
    foreach ($required as $field) {
        if (!isset($input[$field]) || $input[$field] === '') {
            echo json_encode(['success' => false, 'error' => "Missing required field: $field"]);
            return;
        }
    }

    // Generate or use provided ID
    $id = $input['id'] ?? '';
    if (empty($id)) {
        // Auto-generate ID
        $muxers_dir = CONFIG_PATH . '/muxers';
        $max_num = 0;
        if (is_dir($muxers_dir)) {
            $files = glob($muxers_dir . '/mux-*.conf');
            foreach ($files as $file) {
                $filename = basename($file, '.conf');
                if (preg_match('/^mux-(\d+)$/', $filename, $matches)) {
                    $num = intval($matches[1]);
                    if ($num > $max_num) {
                        $max_num = $num;
                    }
                }
            }
        }
        $id = 'mux-' . ($max_num + 1);
    } else {
        $id = preg_replace('/[^a-z0-9_-]/', '', strtolower($id));
    }

    if (empty($id)) {
        echo json_encode(['success' => false, 'error' => 'Invalid ID']);
        return;
    }

    // Check if already exists
    $config_file = CONFIG_PATH . '/muxers/' . $id . '.conf';
    if (file_exists($config_file)) {
        echo json_encode(['success' => false, 'error' => 'Muxer ID already exists']);
        return;
    }

    // Build config
    $config = build_mux_config($id, $input);

    // Ensure directory exists
    if (!is_dir(CONFIG_PATH . '/muxers')) {
        mkdir(CONFIG_PATH . '/muxers', 0755, true);
    }

    // Write config file
    if (!write_mux_config($config_file, $config)) {
        echo json_encode(['success' => false, 'error' => 'Failed to write config file']);
        return;
    }

    echo json_encode(['success' => true, 'id' => $id, 'message' => 'Muxer created successfully']);
}

/**
 * Build muxer config array from input
 */
function build_mux_config($id, $input) {
    $config = [
        'muxer' => [
            'id' => $id,
            'name' => $input['name'],
            'description' => $input['description'] ?? '',
            'enabled' => 'true'
        ],
        'output' => [
            'output_bitrate' => intval($input['output_bitrate']),
            'address' => $input['output_address'],
            'port' => intval($input['output_port'])
        ],
        'tsduck' => [
            'pat_interval' => intval($input['pat_interval'] ?? 100),
            'pmt_interval' => intval($input['pmt_interval'] ?? 100),
            'sdt_interval' => intval($input['sdt_interval'] ?? 500),
            'nit_interval' => intval($input['nit_interval'] ?? 10000)
        ],
        'network' => [
            'network_id' => intval($input['network_id'] ?? 1),
            'network_name' => $input['network_name'] ?? 'CariTrans',
            'ts_id' => intval($input['ts_id'] ?? 1),
            'original_network_id' => intval($input['original_network_id'] ?? 1)
        ],
        'services' => []
    ];

    // Process services (order in array determines PMT order via drag-drop)
    $services = $input['services'] ?? [];
    $i = 1;
    foreach ($services as $service) {
        if (!isset($service['source_id']) || empty($service['source_id'])) {
            continue;
        }

        $prefix = "service.{$i}";
        $config['services']["{$prefix}.enabled"] = 'true';
        $config['services']["{$prefix}.source_type"] = $service['source_type'] ?? 'input';
        $config['services']["{$prefix}.source_id"] = $service['source_id'];
        $config['services']["{$prefix}.source_address"] = $service['source_address'] ?? '';
        $config['services']["{$prefix}.source_port"] = $service['source_port'] ?? '';
        $config['services']["{$prefix}.program_number"] = intval($service['program_number'] ?? ($i * 1000 + 1));
        $config['services']["{$prefix}.service_name"] = $service['service_name'] ?? '';
        $config['services']["{$prefix}.service_provider"] = $service['service_provider'] ?? '';
        $config['services']["{$prefix}.service_type"] = intval($service['service_type'] ?? 0x01);
        $config['services']["{$prefix}.pmt_pid"] = intval($service['pmt_pid'] ?? (256 + ($i - 1)));
        $config['services']["{$prefix}.video_pid"] = intval($service['video_pid'] ?? (100 + ($i - 1) * 100));
        $config['services']["{$prefix}.audio_pid"] = intval($service['audio_pid'] ?? (101 + ($i - 1) * 100));
        $config['services']["{$prefix}.pcr_pid"] = intval($service['pcr_pid'] ?? $config['services']["{$prefix}.video_pid"]);
        $config['services']["{$prefix}.is_pcr_reference"] = ($service['is_pcr_reference'] ?? false) ? 'true' : 'false';

        $i++;
    }

    return $config;
}

/**
 * Write muxer config to INI file
 */
function write_mux_config($file, $config) {
    $content = "# =============================================================================\n";
    $content .= "# CariTranscoder Muxer Configuration (TSDuck)\n";
    $content .= "# Generated: " . date('Y-m-d H:i:s') . "\n";
    $content .= "# =============================================================================\n\n";

    foreach ($config as $section => $values) {
        $content .= "[{$section}]\n";
        foreach ($values as $key => $value) {
            $content .= "{$key} = {$value}\n";
        }
        $content .= "\n";
    }

    return file_put_contents($file, $content) !== false;
}

/**
 * Update existing muxer
 */
function handle_update() {
    $input = json_decode(file_get_contents('php://input'), true);
    if (!$input) {
        echo json_encode(['success' => false, 'error' => 'Invalid JSON input']);
        return;
    }

    $id = $input['id'] ?? '';
    if (empty($id)) {
        echo json_encode(['success' => false, 'error' => 'ID required']);
        return;
    }

    $id = preg_replace('/[^a-zA-Z0-9_-]/', '', $id);
    $config_file = CONFIG_PATH . '/muxers/' . $id . '.conf';

    if (!file_exists($config_file)) {
        echo json_encode(['success' => false, 'error' => 'Muxer not found']);
        return;
    }

    // Stop if running
    $status = get_mux_status($id);
    if ($status === 'running') {
        stop_mux($id);
    }

    // Build and write config
    $config = build_mux_config($id, $input);

    if (!write_mux_config($config_file, $config)) {
        echo json_encode(['success' => false, 'error' => 'Failed to write config file']);
        return;
    }

    echo json_encode(['success' => true, 'message' => 'Muxer updated successfully']);
}

/**
 * Delete muxer
 */
function handle_delete($id) {
    if (empty($id)) {
        echo json_encode(['success' => false, 'error' => 'ID required']);
        return;
    }

    $id = preg_replace('/[^a-zA-Z0-9_-]/', '', $id);
    $config_file = CONFIG_PATH . '/muxers/' . $id . '.conf';

    if (!file_exists($config_file)) {
        echo json_encode(['success' => false, 'error' => 'Muxer not found']);
        return;
    }

    // Stop if running
    stop_mux($id);

    // Delete config file
    if (!unlink($config_file)) {
        echo json_encode(['success' => false, 'error' => 'Failed to delete config file']);
        return;
    }

    echo json_encode(['success' => true, 'message' => 'Muxer deleted successfully']);
}

/**
 * Start muxer using systemd via cari-api
 */
function handle_start($id) {
    if (empty($id)) {
        echo json_encode(['success' => false, 'error' => 'ID required']);
        return;
    }

    $id = preg_replace('/[^a-zA-Z0-9_-]/', '', $id);
    $config_file = CONFIG_PATH . '/muxers/' . $id . '.conf';

    if (!file_exists($config_file)) {
        echo json_encode(['success' => false, 'error' => 'Muxer not found']);
        return;
    }

    // Check if already running
    if (get_mux_status($id) === 'running') {
        echo json_encode(['success' => false, 'error' => 'Muxer already running']);
        return;
    }

    // Start via cari-api (Python FastAPI running as root)
    $result = call_cari_api('/service/control', 'POST', [
        'action' => 'start',
        'service_name' => "cari-mux@$id"
    ]);

    if ($result && isset($result['success']) && $result['success']) {
        echo json_encode(['success' => true, 'message' => 'Muxer started successfully']);
    } else {
        echo json_encode(['success' => false, 'error' => $result['error'] ?? 'Failed to start muxer']);
    }
}

/**
 * Build TSDuck tsp command from config
 */
function build_tsduck_command($id, $config) {
    $output_bitrate = intval($config['output']['output_bitrate'] ?? 20000000);
    $output_address = $config['output']['address'] ?? '';
    $output_port = intval($config['output']['port'] ?? 5000);

    if (empty($output_address)) {
        return null;
    }

    // Start building command
    $cmd_parts = ['tsp', '-b', $output_bitrate];

    // First input: null packet generator (sets overall bitrate)
    $cmd_parts[] = '-I';
    $cmd_parts[] = 'null';
    $cmd_parts[] = '--bitrate';
    $cmd_parts[] = $output_bitrate;

    // Collect services and their inputs
    $services = [];
    $pcr_reference_service = null;

    for ($i = 1; $i <= 20; $i++) {
        $prefix = "service.{$i}";
        if (!isset($config['services']["{$prefix}.enabled"]) ||
            $config['services']["{$prefix}.enabled"] !== 'true') {
            continue;
        }

        $source_address = $config['services']["{$prefix}.source_address"] ?? '';
        $source_port = $config['services']["{$prefix}.source_port"] ?? '';

        if (empty($source_address) || empty($source_port)) {
            continue;
        }

        $service = [
            'index' => $i,
            'address' => $source_address,
            'port' => $source_port,
            'program_number' => intval($config['services']["{$prefix}.program_number"] ?? $i),
            'service_name' => $config['services']["{$prefix}.service_name"] ?? "Service {$i}",
            'service_provider' => $config['services']["{$prefix}.service_provider"] ?? 'CariTrans',
            'service_type' => intval($config['services']["{$prefix}.service_type"] ?? 0x01),
            'pmt_pid' => intval($config['services']["{$prefix}.pmt_pid"] ?? (256 + ($i - 1) * 256)),
            'is_pcr_reference' => ($config['services']["{$prefix}.is_pcr_reference"] ?? 'false') === 'true'
        ];

        $services[] = $service;

        if ($service['is_pcr_reference'] && !$pcr_reference_service) {
            $pcr_reference_service = $service['program_number'];
        }

        // Add input for this service
        $cmd_parts[] = '-I';
        $cmd_parts[] = 'ip';
        $cmd_parts[] = "{$source_address}:{$source_port}";
    }

    if (empty($services)) {
        return null;
    }

    // If no PCR reference specified, use first service
    if (!$pcr_reference_service) {
        $pcr_reference_service = $services[0]['program_number'];
    }

    // Merge plugin
    $cmd_parts[] = '-P';
    $cmd_parts[] = 'merge';

    // PAT plugin - remap services
    $cmd_parts[] = '-P';
    $cmd_parts[] = 'pat';
    foreach ($services as $idx => $service) {
        $cmd_parts[] = '--service';
        $cmd_parts[] = ($idx + 1) . '=' . $service['program_number'];
    }

    // SDT plugin - set service names
    $cmd_parts[] = '-P';
    $cmd_parts[] = 'sdt';
    foreach ($services as $service) {
        $cmd_parts[] = '--service-name';
        $cmd_parts[] = $service['program_number'] . '=' . escapeshellarg($service['service_name']);
        $cmd_parts[] = '--service-provider';
        $cmd_parts[] = $service['program_number'] . '=' . escapeshellarg($service['service_provider']);
    }

    // PCR adjust
    $cmd_parts[] = '-P';
    $cmd_parts[] = 'pcradjust';
    $cmd_parts[] = '--reference-service';
    $cmd_parts[] = $pcr_reference_service;

    // Regulate for CBR output
    $cmd_parts[] = '-P';
    $cmd_parts[] = 'regulate';

    // Bitrate monitor for stats (output to file)
    $stats_file = '/tmp/mux-' . $id . '-stats.json';
    $cmd_parts[] = '-P';
    $cmd_parts[] = 'bitrate_monitor';
    $cmd_parts[] = '--json-line';
    $cmd_parts[] = '--output-file';
    $cmd_parts[] = $stats_file;
    $cmd_parts[] = '--periodic-bitrate';
    $cmd_parts[] = '1000';

    // Output to UDP
    $cmd_parts[] = '-O';
    $cmd_parts[] = 'ip';
    $cmd_parts[] = "{$output_address}:{$output_port}";

    return implode(' ', $cmd_parts);
}

/**
 * Stop muxer
 */
function handle_stop($id) {
    if (empty($id)) {
        echo json_encode(['success' => false, 'error' => 'ID required']);
        return;
    }

    $id = preg_replace('/[^a-zA-Z0-9_-]/', '', $id);

    if (stop_mux($id)) {
        echo json_encode(['success' => true, 'message' => 'Muxer stopped successfully']);
    } else {
        echo json_encode(['success' => false, 'error' => 'Failed to stop muxer']);
    }
}

/**
 * Stop mux process using systemd via cari-api
 */
function stop_mux($id) {
    $result = call_cari_api('/service/control', 'POST', [
        'action' => 'stop',
        'service_name' => "cari-mux@$id"
    ]);

    return $result && isset($result['success']) && $result['success'];
}

/**
 * Get muxer status
 */
function handle_status($id) {
    if (empty($id)) {
        echo json_encode(['success' => false, 'error' => 'ID required']);
        return;
    }

    $id = preg_replace('/[^a-zA-Z0-9_-]/', '', $id);

    $status = get_mux_status($id);
    $pid = null;

    $pid_file = '/run/caritrans/mux-' . $id . '.pid';
    if (file_exists($pid_file)) {
        $pid = trim(file_get_contents($pid_file));
    }

    echo json_encode([
        'success' => true,
        'status' => $status,
        'pid' => $pid
    ]);
}

/**
 * Get muxer metrics (PID bitrates)
 */
function handle_metrics($id) {
    if (empty($id)) {
        echo json_encode(['success' => false, 'error' => 'ID required']);
        return;
    }

    $id = preg_replace('/[^a-zA-Z0-9_-]/', '', $id);
    $stats_file = '/tmp/mux-' . $id . '-stats.json';

    $metrics = [
        'total_bitrate' => 0,
        'pids' => [],
        'services' => [],
        'null_percentage' => 0
    ];

    if (file_exists($stats_file)) {
        // Read last line of stats file (most recent)
        $lines = file($stats_file, FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES);
        if (!empty($lines)) {
            $last_line = end($lines);
            $stats = json_decode($last_line, true);
            if ($stats) {
                $metrics = array_merge($metrics, $stats);
            }
        }
    }

    echo json_encode(['success' => true, 'metrics' => $metrics]);
}

/**
 * Get detailed PID statistics for bandwidth visualization
 */
function handle_pid_stats($id) {
    if (empty($id)) {
        echo json_encode(['success' => false, 'error' => 'ID required']);
        return;
    }

    $id = preg_replace('/[^a-zA-Z0-9_-]/', '', $id);
    $config_file = CONFIG_PATH . '/muxers/' . $id . '.conf';

    if (!file_exists($config_file)) {
        echo json_encode(['success' => false, 'error' => 'Muxer not found']);
        return;
    }

    $config = parse_config($config_file);
    $output_address = $config['output']['address'] ?? '';
    $output_port = $config['output']['port'] ?? '';

    if (empty($output_address) || empty($output_port)) {
        echo json_encode(['success' => false, 'error' => 'No output address configured']);
        return;
    }

    // Use tsp to analyze the output stream
    // Quick 2-second analysis
    $cmd = "timeout 2 tsp -I ip {$output_address}:{$output_port} -P analyze --json -O drop 2>/dev/null";
    exec($cmd, $output, $ret);

    $analysis = null;
    if (!empty($output)) {
        $json_str = implode('', $output);
        $analysis = json_decode($json_str, true);
    }

    $pids = [];
    $total_bitrate = 0;

    if ($analysis && isset($analysis['pids'])) {
        foreach ($analysis['pids'] as $pid_info) {
            $pid = $pid_info['pid'] ?? 0;
            $bitrate = $pid_info['bitrate'] ?? 0;
            $total_bitrate += $bitrate;

            $pids[] = [
                'pid' => $pid,
                'pid_hex' => sprintf('0x%04X', $pid),
                'type' => get_pid_type($pid, $pid_info),
                'bitrate' => $bitrate,
                'percentage' => 0, // Will calculate after totaling
                'description' => $pid_info['description'] ?? ''
            ];
        }
    }

    // Calculate percentages
    if ($total_bitrate > 0) {
        foreach ($pids as &$pid_info) {
            $pid_info['percentage'] = round(($pid_info['bitrate'] / $total_bitrate) * 100, 2);
        }
    }

    // Sort by bitrate descending
    usort($pids, function($a, $b) {
        return $b['bitrate'] - $a['bitrate'];
    });

    echo json_encode([
        'success' => true,
        'total_bitrate' => $total_bitrate,
        'pids' => $pids
    ]);
}

/**
 * Determine PID type for visualization
 */
function get_pid_type($pid, $info = []) {
    // Well-known PIDs
    if ($pid === 0x0000) return 'pat';
    if ($pid === 0x0001) return 'cat';
    if ($pid === 0x0010) return 'nit';
    if ($pid === 0x0011) return 'sdt';
    if ($pid === 0x0012) return 'eit';
    if ($pid === 0x0013) return 'rst';
    if ($pid === 0x0014) return 'tdt';
    if ($pid === 0x1FFF) return 'null';

    // Check description from analysis
    $desc = strtolower($info['description'] ?? '');
    if (strpos($desc, 'video') !== false || strpos($desc, 'avc') !== false ||
        strpos($desc, 'hevc') !== false || strpos($desc, 'mpeg2') !== false) {
        return 'video';
    }
    if (strpos($desc, 'audio') !== false || strpos($desc, 'aac') !== false ||
        strpos($desc, 'ac3') !== false || strpos($desc, 'mp2') !== false) {
        return 'audio';
    }
    if (strpos($desc, 'pmt') !== false) return 'pmt';
    if (strpos($desc, 'pcr') !== false) return 'pcr';
    if (strpos($desc, 'subtitle') !== false || strpos($desc, 'dvb_sub') !== false) {
        return 'subtitle';
    }

    // PMT PIDs are typically in a certain range
    if ($pid >= 0x0100 && $pid < 0x0200) return 'pmt';

    return 'other';
}
