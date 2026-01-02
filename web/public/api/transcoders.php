<?php
/**
 * CariTranscoder - Transcoders API
 * Copyright (c) 2024 CariTech Solutions
 */

define('CARITRANS', true);
require_once __DIR__ . '/../../includes/config.php';
require_once __DIR__ . '/../../includes/auth.php';
require_once __DIR__ . '/../../includes/functions.php';

// API URL for Python backend
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
    case 'probe_stream':
        handle_probe_stream();
        break;
    case 'start_preview':
        handle_start_preview();
        break;
    case 'stop_preview':
        handle_stop_preview();
        break;
    case 'preview_keepalive':
        handle_preview_keepalive();
        break;
    case 'inputs':
        handle_get_inputs();
        break;
    case 'next_id':
        handle_get_next_id();
        break;
    case 'all_metrics':
        handle_all_metrics();
        break;
    case 'metrics_history':
        handle_metrics_history();
        break;
    case 'continuity_errors':
        handle_continuity_errors();
        break;
    case 'list':
    default:
        handle_list();
        break;
}

/**
 * Call the CariTranscoder Python API
 */
function call_cari_api($endpoint, $method = 'GET', $data = null) {
    $url = CARI_API_URL . $endpoint;

    $options = [
        'http' => [
            'method' => $method,
            'timeout' => 30,
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
        return ['success' => false, 'error' => 'Invalid response from API: ' . substr($response, 0, 200)];
    }

    return $result;
}

/**
 * Get list of available inputs for dropdown
 */
function handle_get_inputs() {
    try {
        $inputs = get_service_list('inputs');
        $result = [];

        foreach ($inputs as $input) {
            // Get the output address from input config - use CONFIG_PATH like get_service_list does
            $config_file = CONFIG_PATH . '/inputs/' . $input['id'] . '.conf';
            $output_address = '';
            $output_port = '';

            if (file_exists($config_file)) {
                $config = parse_config($config_file);
                $output_address = $config['output']['address'] ?? '';
                $output_port = $config['output']['port'] ?? '';
            }

            $result[] = [
                'id' => $input['id'],
                'name' => $input['name'],
                'output_address' => $output_address,
                'output_port' => $output_port
            ];
        }

        echo json_encode(['success' => true, 'inputs' => $result]);
    } catch (Exception $e) {
        echo json_encode(['success' => false, 'error' => $e->getMessage()]);
    }
}

/**
 * Get next available transcoder ID number for an input
 */
function handle_get_next_id() {
    $input_id = $_GET['input_id'] ?? '';
    if (empty($input_id)) {
        echo json_encode(['success' => false, 'error' => 'Input ID required']);
        return;
    }

    // Clean input_id for use in transcoder name
    $base_id = preg_replace('/[^a-z0-9_-]/', '', strtolower($input_id));

    // Find existing transcoders for this input
    $transcoders_dir = CONFIG_PATH . '/transcoders';
    $max_num = 0;

    if (is_dir($transcoders_dir)) {
        // Look for both old format (_trans_) and new format (_transcoder_)
        $files = array_merge(
            glob($transcoders_dir . '/' . $base_id . '_transcoder_*.conf'),
            glob($transcoders_dir . '/' . $base_id . '_trans_*.conf')
        );
        foreach ($files as $file) {
            $filename = basename($file, '.conf');
            if (preg_match('/_(?:transcoder|trans)_(\d+)$/', $filename, $matches)) {
                $num = intval($matches[1]);
                if ($num > $max_num) {
                    $max_num = $num;
                }
            }
        }
    }

    $next_num = $max_num + 1;
    $next_id = $base_id . '_transcoder_' . $next_num;
    $next_name = $base_id . '_transcoder_' . $next_num;

    echo json_encode([
        'success' => true,
        'next_number' => $next_num,
        'next_id' => $next_id,
        'next_name' => $next_name
    ]);
}

/**
 * List all transcoders
 */
function handle_list() {
    $transcoders = get_service_list('transcoders');
    echo json_encode(['success' => true, 'transcoders' => $transcoders]);
}

/**
 * Get a single transcoder config
 */
function handle_get($id) {
    if (empty($id)) {
        echo json_encode(['success' => false, 'error' => 'ID required']);
        return;
    }

    $id = preg_replace('/[^a-zA-Z0-9_-]/', '', $id);
    $config_file = CONFIG_PATH . '/transcoders/' . $id . '.conf';

    if (!file_exists($config_file)) {
        echo json_encode(['success' => false, 'error' => 'Transcoder not found']);
        return;
    }

    $config = parse_config($config_file);
    echo json_encode(['success' => true, 'config' => $config]);
}

/**
 * Create a new transcoder
 */
function handle_create() {
    $input = json_decode(file_get_contents('php://input'), true);
    if (!$input) {
        echo json_encode(['success' => false, 'error' => 'Invalid JSON input']);
        return;
    }

    // Validate required fields
    $required = ['name', 'transcoder_id', 'input_address', 'input_port', 'output_address', 'output_port'];
    foreach ($required as $field) {
        if (empty($input[$field])) {
            echo json_encode(['success' => false, 'error' => "Missing required field: $field"]);
            return;
        }
    }

    $id = preg_replace('/[^a-z0-9_-]/', '', strtolower($input['transcoder_id']));
    if (empty($id)) {
        echo json_encode(['success' => false, 'error' => 'Invalid ID']);
        return;
    }

    // Check if already exists
    $config_file = CONFIG_PATH . '/transcoders/' . $id . '.conf';
    if (file_exists($config_file)) {
        echo json_encode(['success' => false, 'error' => 'Transcoder ID already exists']);
        return;
    }

    // Build config
    $config = build_transcoder_config($input);

    // Ensure directory exists
    if (!is_dir(CONFIG_PATH . '/transcoders')) {
        mkdir(CONFIG_PATH . '/transcoders', 0755, true);
    }

    // Write config file
    if (!write_ini_file($config_file, $config)) {
        echo json_encode(['success' => false, 'error' => 'Failed to write config file']);
        return;
    }

    // Create systemd service (optional - may fail in dev mode if API not running)
    $result = create_transcoder_service($id, $config);
    if (!$result['success']) {
        // In dev mode, still succeed if config was saved (service creation is optional)
        if (DEV_MODE) {
            error_log("Transcoder service creation skipped (dev mode): " . ($result['error'] ?? 'API unavailable'));
            echo json_encode(['success' => true, 'id' => $id, 'message' => 'Transcoder config saved (service creation skipped in dev mode)']);
            return;
        }
        // In production, clean up config file on failure
        unlink($config_file);
        echo json_encode($result);
        return;
    }

    echo json_encode(['success' => true, 'id' => $id, 'message' => 'Transcoder created successfully']);
}

/**
 * Update an existing transcoder
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
    $config_file = CONFIG_PATH . '/transcoders/' . $id . '.conf';

    if (!file_exists($config_file)) {
        echo json_encode(['success' => false, 'error' => 'Transcoder not found']);
        return;
    }

    // Build config
    $config = build_transcoder_config($input);

    // Write config file
    if (!write_ini_file($config_file, $config)) {
        echo json_encode(['success' => false, 'error' => 'Failed to write config file']);
        return;
    }

    // Stop existing service (ignore errors in dev mode)
    if (!DEV_MODE) {
        stop_transcoder_service($id);
    }

    // Recreate systemd service (optional in dev mode)
    $result = create_transcoder_service($id, $config);
    if (!$result['success']) {
        if (DEV_MODE) {
            error_log("Transcoder service update skipped (dev mode): " . ($result['error'] ?? 'API unavailable'));
            echo json_encode(['success' => true, 'message' => 'Transcoder config updated (service update skipped in dev mode)']);
            return;
        }
        echo json_encode($result);
        return;
    }

    echo json_encode(['success' => true, 'message' => 'Transcoder updated successfully']);
}

/**
 * Delete a transcoder
 */
function handle_delete($id) {
    if (empty($id)) {
        echo json_encode(['success' => false, 'error' => 'ID required']);
        return;
    }

    $id = preg_replace('/[^a-zA-Z0-9_-]/', '', $id);
    $config_file = CONFIG_PATH . '/transcoders/' . $id . '.conf';

    // Stop and disable service first
    stop_transcoder_service($id);
    delete_transcoder_service($id);

    // Delete config file
    if (file_exists($config_file)) {
        unlink($config_file);
    }

    echo json_encode(['success' => true, 'message' => 'Transcoder deleted']);
}

/**
 * Start a transcoder
 */
function handle_start($id) {
    if (empty($id)) {
        echo json_encode(['success' => false, 'error' => 'ID required']);
        return;
    }

    $result = call_cari_api('/service/control', 'POST', [
        'action' => 'start',
        'service_name' => "cari-transcoder@$id"
    ]);

    if ($result && isset($result['success']) && $result['success']) {
        echo json_encode(['success' => true, 'message' => 'Transcoder started']);
    } else {
        echo json_encode(['success' => false, 'error' => $result['error'] ?? 'Failed to start transcoder']);
    }
}

/**
 * Stop a transcoder
 */
function handle_stop($id) {
    if (empty($id)) {
        echo json_encode(['success' => false, 'error' => 'ID required']);
        return;
    }

    $result = stop_transcoder_service($id);
    echo json_encode($result);
}

/**
 * Get transcoder status
 */
function handle_status($id) {
    if (empty($id)) {
        echo json_encode(['success' => false, 'error' => 'ID required']);
        return;
    }

    $result = call_cari_api('/service/control', 'POST', [
        'action' => 'status',
        'service_name' => "cari-transcoder@$id"
    ]);

    echo json_encode($result ?: ['success' => false, 'error' => 'Failed to get status']);
}

/**
 * Build transcoder config array from input
 */
function build_transcoder_config($input) {
    $is_abr = !empty($input['abr_enabled']);

    $config = [
        'general' => [
            'name' => $input['name'] ?? '',
            'enabled' => true,
            'abr_enabled' => $is_abr
        ],
        'input' => [
            'address' => $input['input_address'] ?? '',
            'port' => intval($input['input_port'] ?? 5000)
        ],
        'output' => [
            'address' => $input['output_address'] ?? '',
            'port' => intval($input['output_port'] ?? 5000),
            'api_port' => intval($input['api_port'] ?? 9200),
            'video_pid' => intval($input['video_pid'] ?? 256),
            'audio_pid' => intval($input['audio_pid'] ?? 257),
            'program_number' => intval($input['program_number'] ?? 1)
        ],
        'video' => [
            'mode' => $input['video_mode'] ?? 'transcode',
            'codec' => $input['video_codec'] ?? 'h264',
            'encoder_type' => 'cpu',
            'bitrate' => intval($input['video_bitrate'] ?? 5000000),
            'preset' => $input['video_preset'] ?? 'superfast',
            'keyframe_interval' => intval($input['keyframe_interval'] ?? 60),
            'profile' => $input['video_profile'] ?? 'main',
            'bframes' => intval($input['bframes'] ?? 0),
            'ref' => intval($input['ref'] ?? 1),
            'qp_min' => intval($input['qp_min'] ?? 10),
            'qp_max' => intval($input['qp_max'] ?? 51),
            'vbv_bufsize' => intval($input['vbv_bufsize'] ?? 600),
            'threads' => intval($input['video_threads'] ?? 0),
            'sliced_threads' => !empty($input['sliced_threads']),
            'cabac' => !empty($input['cabac']),
            'trellis' => !empty($input['trellis']),
            'aud' => !empty($input['aud']),
            'intra_refresh' => !empty($input['intra_refresh']),
            'interlaced' => !empty($input['interlaced']),
            'psy_tune' => $input['psy_tune'] ?? '',
            'x264_opts' => $input['x264_opts'] ?? ''
        ],
        'scaling' => [
            'enabled' => !empty($input['scaling_enabled']),
            'width' => intval($input['scale_width'] ?? 1920),
            'height' => intval($input['scale_height'] ?? 1080),
            'method' => intval($input['scale_method'] ?? 1),
            'add_borders' => !empty($input['add_borders']),
            'threads' => intval($input['scale_threads'] ?? 0),
            'deinterlace' => !empty($input['deinterlace'])
        ],
        'audio' => [
            'mode' => $input['audio_mode'] ?? 'transcode',
            'codec' => $input['audio_codec'] ?? 'aac',
            'bitrate' => intval($input['audio_bitrate'] ?? 128000),
            'channels' => intval($input['audio_channels'] ?? 2),
            'samplerate' => intval($input['audio_samplerate'] ?? 48000)
        ]
    ];

    // Handle ABR variants
    if ($is_abr && isset($input['variants']) && is_array($input['variants'])) {
        $config['abr'] = [
            'enabled' => true,
            'variant_count' => count($input['variants'])
        ];
        foreach ($input['variants'] as $i => $variant) {
            $config['abr']["variant_{$i}_width"] = intval($variant['width'] ?? 1920);
            $config['abr']["variant_{$i}_height"] = intval($variant['height'] ?? 1080);
            $config['abr']["variant_{$i}_bitrate"] = intval($variant['bitrate'] ?? 5000000);
            $config['abr']["variant_{$i}_video_pid"] = intval($variant['video_pid'] ?? (100 + $i * 100));
        }
    }

    return $config;
}

/**
 * Write INI file from config array
 */
function write_ini_file($file, $config) {
    $content = "";
    foreach ($config as $section => $values) {
        $content .= "[$section]\n";
        foreach ($values as $key => $value) {
            if (is_bool($value)) {
                $value = $value ? 'true' : 'false';
            }
            $content .= "$key = $value\n";
        }
        $content .= "\n";
    }
    return file_put_contents($file, $content) !== false;
}

/**
 * Create transcoder systemd service via Python API
 */
function create_transcoder_service($id, $config) {
    $video = $config['video'];
    $audio = $config['audio'];
    $scaling = $config['scaling'];
    $input = $config['input'];
    $output = $config['output'];
    $general = $config['general'];
    $is_abr = !empty($general['abr_enabled']) && isset($config['abr']);

    // Calculate tsp bitrate (video + audio + 5% overhead)
    if ($is_abr) {
        // For ABR, sum all variant bitrates
        $total_video_bitrate = 0;
        $variant_count = $config['abr']['variant_count'] ?? 0;
        for ($i = 0; $i < $variant_count; $i++) {
            $total_video_bitrate += $config['abr']["variant_{$i}_bitrate"] ?? 0;
        }
        $total_bitrate = $total_video_bitrate + $audio['bitrate'];
    } else {
        $total_bitrate = $video['bitrate'] + $audio['bitrate'];
    }
    $tsp_bitrate = intval($total_bitrate * 1.05);

    // Build service data for Python API
    $service_data = [
        'id' => $id,
        'name' => $config['general']['name'],
        'input_address' => $input['address'],
        'input_port' => $input['port'],
        'output_address' => $output['address'],
        'output_port' => $output['port'],
        'api_port' => $output['api_port'],
        'video_pid' => $output['video_pid'] ?? 256,
        'audio_pid' => $output['audio_pid'] ?? 257,
        'program_number' => $output['program_number'] ?? 1,
        'tsp_bitrate' => $tsp_bitrate,

        // Video settings
        'video_mode' => $video['mode'],
        'video_codec' => $video['codec'],
        'video_bitrate' => $video['bitrate'],
        'video_preset' => $video['preset'],
        'keyframe_interval' => $video['keyframe_interval'],
        'profile' => $video['profile'],
        'bframes' => $video['bframes'],
        'ref' => $video['ref'],
        'qp_min' => $video['qp_min'],
        'qp_max' => $video['qp_max'],
        'vbv_bufsize' => $video['vbv_bufsize'],
        'video_threads' => $video['threads'],
        'sliced_threads' => $video['sliced_threads'],
        'cabac' => $video['cabac'],
        'trellis' => $video['trellis'],
        'aud' => $video['aud'],
        'intra_refresh' => $video['intra_refresh'],
        'interlaced' => $video['interlaced'],
        'psy_tune' => $video['psy_tune'],
        'x264_opts' => $video['x264_opts'],

        // Scaling settings
        'scaling_enabled' => $scaling['enabled'],
        'scale_width' => $scaling['width'],
        'scale_height' => $scaling['height'],
        'scale_method' => $scaling['method'],
        'add_borders' => $scaling['add_borders'],
        'scale_threads' => $scaling['threads'],
        'deinterlace' => $scaling['deinterlace'],

        // Audio settings
        'audio_mode' => $audio['mode'],
        'audio_codec' => $audio['codec'],
        'audio_bitrate' => $audio['bitrate'],
        'audio_channels' => $audio['channels'],
        'audio_samplerate' => $audio['samplerate'],

        // ABR settings
        'abr_enabled' => $is_abr
    ];

    // Add ABR variants if enabled
    if ($is_abr) {
        $variants = [];
        $variant_count = $config['abr']['variant_count'] ?? 0;
        for ($i = 0; $i < $variant_count; $i++) {
            $variants[] = [
                'width' => $config['abr']["variant_{$i}_width"] ?? 1920,
                'height' => $config['abr']["variant_{$i}_height"] ?? 1080,
                'bitrate' => $config['abr']["variant_{$i}_bitrate"] ?? 5000000,
                'video_pid' => $config['abr']["variant_{$i}_video_pid"] ?? (100 + $i * 100)
            ];
        }
        $service_data['variants'] = $variants;
    }

    $result = call_cari_api('/transcoder/create', 'POST', $service_data);

    if (!$result || !isset($result['success']) || !$result['success']) {
        return ['success' => false, 'error' => $result['error'] ?? 'Failed to create service'];
    }

    return ['success' => true];
}

/**
 * Stop transcoder service
 */
function stop_transcoder_service($id) {
    $result = call_cari_api('/service/control', 'POST', [
        'action' => 'stop',
        'service_name' => "cari-transcoder@$id"
    ]);

    return $result ?: ['success' => false, 'error' => 'Failed to stop service'];
}

/**
 * Delete transcoder service
 */
function delete_transcoder_service($id) {
    $result = call_cari_api('/transcoder/delete', 'POST', ['id' => $id]);
    return $result ?: ['success' => false, 'error' => 'Failed to delete service'];
}

/**
 * Probe a stream using ffprobe API
 * Returns video/audio codec info, resolution, etc.
 */
function handle_probe_stream() {
    $address = $_GET['address'] ?? '';

    if (empty($address)) {
        echo json_encode(['success' => false, 'error' => 'Stream address required']);
        return;
    }

    // Build the stream URL - expect format like "239.1.1.1:5000"
    // Convert to udp://@ADDRESS:PORT format for ffprobe
    $stream_url = 'udp://@' . $address;

    $api_data = [
        'stream_url' => $stream_url
    ];

    $result = call_cari_api('/preview/media-info', 'POST', $api_data);

    echo json_encode($result ?: ['success' => false, 'error' => 'Failed to probe stream']);
}

/**
 * Start player_preview for transcoder output
 */
function handle_start_preview() {
    $input = json_decode(file_get_contents('php://input'), true);

    $id = $input['id'] ?? '';
    $input_address = $input['input_address'] ?? '';
    $output_dir = $input['output_dir'] ?? '';
    $api_port = intval($input['api_port'] ?? 0);

    if (empty($input_address) || empty($output_dir) || $api_port === 0) {
        echo json_encode(['success' => false, 'error' => 'Missing required parameters']);
        return;
    }

    $api_data = [
        'input_address' => $input_address,
        'output_dir' => $output_dir,
        'api_port' => $api_port,
        'folder' => 'transcoder-' . $id
    ];

    $result = call_cari_api('/preview/start', 'POST', $api_data);

    echo json_encode($result ?: ['success' => false, 'error' => 'Failed to start preview']);
}

/**
 * Stop player_preview for transcoder output
 */
function handle_stop_preview() {
    $api_port = intval($_GET['api_port'] ?? 0);

    if ($api_port === 0) {
        echo json_encode(['success' => false, 'error' => 'API port required']);
        return;
    }

    $result = call_cari_api("/preview/stop/{$api_port}", 'POST');

    echo json_encode($result ?: ['success' => false, 'error' => 'Failed to stop preview']);
}

/**
 * Send keepalive to player_preview
 */
function handle_preview_keepalive() {
    $api_port = intval($_GET['api_port'] ?? 0);

    if ($api_port === 0) {
        echo json_encode(['success' => false, 'error' => 'API port required']);
        return;
    }

    // Send keepalive directly to preview port
    $ctx = stream_context_create([
        'http' => [
            'method' => 'POST',
            'timeout' => 2,
            'ignore_errors' => true
        ]
    ]);

    $response = @file_get_contents("http://127.0.0.1:{$api_port}/keepalive", false, $ctx);

    if ($response !== false) {
        echo json_encode(['success' => true]);
    } else {
        echo json_encode(['success' => false, 'error' => 'Keepalive failed']);
    }
}

/**
 * Get metrics for all transcoders
 * Parses tsp bitrate_monitor output from log files
 */
function handle_all_metrics() {
    $transcoders = get_service_list('transcoders');
    $all_metrics = [];

    foreach ($transcoders as $transcoder) {
        $id = $transcoder['id'];
        $metrics = get_transcoder_metrics($id);
        $all_metrics[$id] = $metrics;
    }

    echo json_encode(['success' => true, 'transcoders' => $all_metrics]);
}

/**
 * Get historical metrics for a transcoder
 * Parses the full log file for bitrate history
 */
function handle_metrics_history() {
    $id = $_GET['id'] ?? '';
    if (empty($id)) {
        echo json_encode(['success' => false, 'error' => 'Transcoder ID required']);
        return;
    }

    $id = preg_replace('/[^a-zA-Z0-9_-]/', '', $id);
    $log_file = "/var/log/caritrans/transcoder-{$id}.log";
    $config_file = CONFIG_PATH . '/transcoders/' . $id . '.conf';

    if (!file_exists($config_file)) {
        echo json_encode(['success' => false, 'error' => 'Transcoder not found']);
        return;
    }

    $config = parse_config($config_file);
    $audio_pid = intval($config['output']['audio_pid'] ?? 257);

    // Check for ABR mode with multiple video PIDs
    $is_abr = !empty($config['abr']['enabled']) && config_bool($config['abr']['enabled']);
    $video_pids = [];

    if ($is_abr && isset($config['abr']['variant_count'])) {
        $variant_count = intval($config['abr']['variant_count']);
        for ($i = 0; $i < $variant_count; $i++) {
            $pid = intval($config['abr']["variant_{$i}_video_pid"] ?? (100 + $i * 100));
            $video_pids[] = $pid;
        }
    }

    // Fall back to single video PID if not ABR or no variants
    if (empty($video_pids)) {
        $video_pids[] = intval($config['output']['video_pid'] ?? 256);
    }

    // Build initial history structure
    $pids_data = [];
    foreach ($video_pids as $pid) {
        $pids_data[$pid] = ['history' => [], 'is_video' => true];
    }
    $pids_data[$audio_pid] = ['history' => [], 'is_video' => false];

    $history = [
        'success' => true,
        'transcoder_id' => $id,
        'video_pids' => $video_pids,
        'video_pid' => $video_pids[0],  // First video PID for backwards compatibility
        'audio_pid' => $audio_pid,
        'is_abr' => $is_abr,
        'pids' => $pids_data
    ];

    if (!file_exists($log_file) || !is_readable($log_file)) {
        echo json_encode($history);
        return;
    }

    // Read the entire log file (or last N KB for performance)
    $max_bytes = 256 * 1024; // Read last 256KB
    $file_size = filesize($log_file);
    $fp = fopen($log_file, 'r');
    if (!$fp) {
        echo json_encode($history);
        return;
    }

    // Seek to near end if file is large
    if ($file_size > $max_bytes) {
        fseek($fp, $file_size - $max_bytes);
        fgets($fp); // Skip partial line
    }

    // Initialize history arrays for each PID
    $pid_histories = [];
    foreach ($video_pids as $pid) {
        $pid_histories[$pid] = [];
    }
    $pid_histories[$audio_pid] = [];

    // Parse bitrate_monitor output lines
    // Format: * bitrate_monitor: YYYY/MM/DD HH:MM:SS, PID 0x0100 (256) bitrate: 5,384,620 bits/s
    while (!feof($fp)) {
        $line = fgets($fp);
        if ($line === false) break;

        if (strpos($line, 'bitrate_monitor') !== false &&
            preg_match('/bitrate_monitor:\s*(\d{4}\/\d{2}\/\d{2}\s+\d{2}:\d{2}:\d{2}),\s*PID\s+0x[0-9a-fA-F]+\s+\((\d+)\)\s+bitrate:\s+([\d,]+)\s+bits\/s/', $line, $matches)) {

            $timestamp_str = $matches[1];
            $pid = intval($matches[2]);
            $bitrate = intval(str_replace(',', '', $matches[3]));

            // Convert to unix timestamp
            $timestamp = strtotime(str_replace('/', '-', $timestamp_str));

            // Store in appropriate history array if this PID is tracked
            if (isset($pid_histories[$pid])) {
                $pid_histories[$pid][] = [$timestamp, $bitrate];
            }
        }
    }
    fclose($fp);

    // Keep last 300 samples (about 10 minutes at 2-second intervals)
    $max_samples = 300;
    foreach ($pid_histories as $pid => $hist) {
        if (count($hist) > $max_samples) {
            $pid_histories[$pid] = array_slice($hist, -$max_samples);
        }
        $history['pids'][$pid]['history'] = $pid_histories[$pid];
    }

    echo json_encode($history);
}

/**
 * Get metrics for a single transcoder
 * Parses the last bitrate_monitor output from the log file
 * Also fetches input bitrate from linked input service
 */
function get_transcoder_metrics($id) {
    $log_file = "/var/log/caritrans/transcoder-{$id}.log";
    $config_file = CONFIG_PATH . '/transcoders/' . $id . '.conf';
    $service_file = "/etc/systemd/system/cari-transcoder@{$id}.service";

    $metrics = [
        'status' => 'offline',
        'output_video_bitrate' => 0,
        'output_audio_bitrate' => 0,
        'output_total_bitrate' => 0,
        'input_video_bitrate' => 0,
        'input_audio_bitrate' => 0,
        'input_format' => null,
        'output_format' => null,
        'video_pid' => 256,
        'video_pids' => [],
        'audio_pid' => 257,
        'is_abr' => false,
        'video_bitrates_by_pid' => [],
        'continuity_errors' => 0,
        'continuity_errors_by_pid' => []
    ];

    $input_api_port = null;
    $input_address = null;
    $video_pids = [];

    // Try to get input source from transcoder config file first
    if (file_exists($config_file)) {
        $config = parse_config($config_file);

        // Check for ABR mode with multiple video PIDs
        $is_abr = !empty($config['abr']['enabled']) && config_bool($config['abr']['enabled']);
        $metrics['is_abr'] = $is_abr;

        if ($is_abr && isset($config['abr']['variant_count'])) {
            $variant_count = intval($config['abr']['variant_count']);
            for ($i = 0; $i < $variant_count; $i++) {
                $pid = intval($config['abr']["variant_{$i}_video_pid"] ?? (100 + $i * 100));
                $video_pids[] = $pid;
            }
            $metrics['video_pids'] = $video_pids;
        }

        // Fall back to single video PID if not ABR or no variants
        if (empty($video_pids)) {
            $video_pids[] = intval($config['output']['video_pid'] ?? 256);
        }

        // Get configured PIDs and api_port for output stream
        $metrics['video_pid'] = $video_pids[0];  // First video PID for backwards compatibility
        $metrics['audio_pid'] = intval($config['output']['audio_pid'] ?? 257);
        $metrics['api_port'] = intval($config['output']['api_port'] ?? 9200);

        // Get output format from config
        $metrics['output_format'] = [
            'video_codec' => $config['video']['codec'] ?? 'unknown',
            'video_bitrate' => $config['video']['bitrate'] ?? 0,
            'video_resolution' => ($config['video']['width'] ?? 'auto') . 'x' . ($config['video']['height'] ?? 'auto'),
            'audio_codec' => $config['audio']['codec'] ?? 'aac',
            'audio_bitrate' => $config['audio']['bitrate'] ?? 128000,
            'audio_channels' => $config['audio']['channels'] ?? 2
        ];

        // Get linked input service
        $source_service = $config['input']['source_service'] ?? null;
        if ($source_service) {
            $metrics['source_service'] = $source_service;
            $input_config_file = CONFIG_PATH . '/inputs/' . $source_service . '.conf';
            if (file_exists($input_config_file)) {
                $input_config = parse_config($input_config_file);
                $input_api_port = $input_config['output']['api_port'] ?? null;
            }
        }
    }

    // If no config file or no source_service, try to parse input address from service file
    if (file_exists($service_file)) {
        $service_content = file_get_contents($service_file);

        // Parse --input ADDRESS:PORT from the ExecStart line
        if (!$input_api_port && preg_match('/--input\s+(\d+\.\d+\.\d+\.\d+):(\d+)/', $service_content, $matches)) {
            $input_address = $matches[1];
            $input_port = $matches[2];
            $metrics['input_address'] = $input_address . ':' . $input_port;

            // Find an input whose output matches this address:port
            $inputs_dir = CONFIG_PATH . '/inputs';
            if (is_dir($inputs_dir)) {
                foreach (glob("{$inputs_dir}/*.conf") as $input_file) {
                    $input_config = parse_config($input_file);
                    $out_addr = $input_config['output']['address'] ?? '';
                    $out_port = $input_config['output']['port'] ?? '';

                    if ($out_addr === $input_address && $out_port == $input_port) {
                        $input_api_port = $input_config['output']['api_port'] ?? null;
                        $input_id = basename($input_file, '.conf');
                        $metrics['source_service'] = $input_id;
                        break;
                    }
                }
            }
        }

        // Parse output address: --udp-host HOST --udp-port PORT (new format)
        // or -O ip ADDRESS:PORT (legacy format)
        if (preg_match('/--udp-host\s+(\d+\.\d+\.\d+\.\d+)\s+--udp-port\s+(\d+)/', $service_content, $matches)) {
            $metrics['output_address'] = $matches[1] . ':' . $matches[2];
        } elseif (preg_match('/-O\s+ip\s+(\d+\.\d+\.\d+\.\d+):(\d+)/', $service_content, $matches)) {
            $metrics['output_address'] = $matches[1] . ':' . $matches[2];
        }
    }

    // Fetch input metrics if we found an API port
    if ($input_api_port) {
        $url = "http://127.0.0.1:{$input_api_port}/metrics";
        $ctx = stream_context_create([
            'http' => ['timeout' => 2, 'ignore_errors' => true]
        ]);
        $response = @file_get_contents($url, false, $ctx);

        if ($response !== false) {
            $input_data = json_decode($response, true);
            if ($input_data && isset($input_data['pids'])) {
                // Process PIDs to get video/audio bitrate
                foreach ($input_data['pids'] as $pid => $pidData) {
                    $bitrate = $pidData['current_bitrate'] ?? 0;
                    if ($bitrate > 500000 && $metrics['input_video_bitrate'] === 0) {
                        $metrics['input_video_bitrate'] = $bitrate;
                    } elseif ($bitrate > 0 && $bitrate <= 500000) {
                        $metrics['input_audio_bitrate'] += $bitrate;
                    }
                }

                // Get input format from stream info
                if (isset($input_data['stream_info'])) {
                    $metrics['input_format'] = $input_data['stream_info'];
                }
            }
        }
    }

    // Check if service is running
    $service_name = "cari-transcoder@{$id}";
    exec("systemctl is-active " . escapeshellarg($service_name) . " 2>/dev/null", $output, $retval);
    $is_running = ($retval === 0 && isset($output[0]) && trim($output[0]) === 'active');

    if (!$is_running) {
        $metrics['status'] = 'stopped';
        return $metrics;
    }

    $metrics['status'] = 'running';

    // Try to read output bitrate from log file (last few lines)
    if (file_exists($log_file) && is_readable($log_file)) {
        // Read last 20 lines of log file
        $lines = [];
        $fp = fopen($log_file, 'r');
        if ($fp) {
            // Seek to approximate position near end (last ~4KB)
            fseek($fp, max(0, filesize($log_file) - 4096));
            fgets($fp); // Skip partial line
            while (!feof($fp)) {
                $line = fgets($fp);
                if ($line !== false) {
                    $lines[] = $line;
                }
            }
            fclose($fp);

            // Keep only last 20 lines
            $lines = array_slice($lines, -20);
        }

        // Parse bitrate_monitor output
        // Format with --pid: * bitrate_monitor: YYYY/MM/DD HH:MM:SS, PID 0x0100 (256) bitrate: 5,384,620 bits/s
        // Note: Numbers may contain commas as thousand separators

        $audio_pid = $metrics['audio_pid'];

        // For ABR mode, we need to track each video PID's bitrate
        $video_bitrates = [];
        foreach ($video_pids as $pid) {
            $video_bitrates[$pid] = 0;
        }

        foreach (array_reverse($lines) as $line) {
            if (strpos($line, 'bitrate_monitor') !== false) {
                // Try per-PID format - match the configured PIDs
                if (preg_match('/PID\s+0x[0-9a-fA-F]+\s+\((\d+)\)\s+bitrate:\s+([\d,]+)\s+bits\/s/', $line, $matches)) {
                    $pid = intval($matches[1]);
                    $bitrate = intval(str_replace(',', '', $matches[2]));

                    // Check if this is one of our video PIDs
                    if (in_array($pid, $video_pids) && $video_bitrates[$pid] === 0) {
                        $video_bitrates[$pid] = $bitrate;
                    } elseif ($pid === $audio_pid && $metrics['output_audio_bitrate'] === 0) {
                        $metrics['output_audio_bitrate'] = $bitrate;
                    }
                }
                // Fallback: try total TS bitrate format (for backwards compatibility)
                elseif (preg_match('/TS bitrate:\s*([\d,]+)\s*bits\/s/', $line, $matches)) {
                    $total_bitrate = intval(str_replace(',', '', $matches[1]));
                    if ($metrics['output_total_bitrate'] === 0) {
                        $metrics['output_total_bitrate'] = $total_bitrate;
                    }
                }
            }
        }

        // Store per-PID bitrates and calculate total video bitrate
        $metrics['video_bitrates_by_pid'] = $video_bitrates;
        $total_video_bitrate = array_sum($video_bitrates);
        $metrics['output_video_bitrate'] = $total_video_bitrate;

        // Calculate total from video + audio if we have per-PID values
        if ($total_video_bitrate > 0 || $metrics['output_audio_bitrate'] > 0) {
            $metrics['output_total_bitrate'] = $total_video_bitrate + $metrics['output_audio_bitrate'];
        }

        // Parse CONTINUITY errors from log
        // Format: Warning from tsdemux0: CONTINUITY: TS packet continuity error (pid:256 (0x0100) )
        foreach ($lines as $line) {
            if (strpos($line, 'CONTINUITY') !== false && strpos($line, 'continuity error') !== false) {
                $metrics['continuity_errors']++;
                // Extract PID from the message
                if (preg_match('/pid:(\d+)/', $line, $matches)) {
                    $pid = intval($matches[1]);
                    if (!isset($metrics['continuity_errors_by_pid'][$pid])) {
                        $metrics['continuity_errors_by_pid'][$pid] = 0;
                    }
                    $metrics['continuity_errors_by_pid'][$pid]++;
                }
            }
        }
    }

    // For backwards compatibility, also set video_bitrate/audio_bitrate
    $metrics['video_bitrate'] = $metrics['output_video_bitrate'];
    $metrics['audio_bitrate'] = $metrics['output_audio_bitrate'];

    return $metrics;
}

/**
 * Handle continuity errors request
 * Returns all continuity errors from the log file with timestamps
 */
function handle_continuity_errors() {
    $id = $_GET['id'] ?? '';
    if (empty($id)) {
        echo json_encode(['success' => false, 'error' => 'Transcoder ID required']);
        return;
    }

    $id = preg_replace('/[^a-zA-Z0-9_-]/', '', $id);
    $log_file = "/var/log/caritrans/transcoder-{$id}.log";

    if (!file_exists($log_file) || !is_readable($log_file)) {
        echo json_encode([
            'success' => true,
            'total_errors' => 0,
            'errors_by_pid' => [],
            'recent_errors' => []
        ]);
        return;
    }

    // Read last 100KB of log file for analysis
    $errors_by_pid = [];
    $recent_errors = [];
    $total_errors = 0;

    $fp = fopen($log_file, 'r');
    if ($fp) {
        // Seek to position near end (last 100KB)
        $file_size = filesize($log_file);
        $read_size = min(102400, $file_size);
        fseek($fp, max(0, $file_size - $read_size));

        if ($file_size > $read_size) {
            fgets($fp); // Skip partial line
        }

        while (!feof($fp)) {
            $line = fgets($fp);
            if ($line === false) continue;

            // Parse: Warning from tsdemux0: CONTINUITY: TS packet continuity error (pid:256 (0x0100) )
            if (strpos($line, 'CONTINUITY') !== false && strpos($line, 'continuity error') !== false) {
                $total_errors++;

                // Extract PID
                $pid = 0;
                if (preg_match('/pid:(\d+)/', $line, $matches)) {
                    $pid = intval($matches[1]);
                    if (!isset($errors_by_pid[$pid])) {
                        $errors_by_pid[$pid] = 0;
                    }
                    $errors_by_pid[$pid]++;
                }

                // Store recent errors with timestamp (keep last 50)
                $recent_errors[] = [
                    'pid' => $pid,
                    'message' => trim($line),
                    'time' => time()
                ];
                if (count($recent_errors) > 50) {
                    array_shift($recent_errors);
                }
            }
        }
        fclose($fp);
    }

    echo json_encode([
        'success' => true,
        'total_errors' => $total_errors,
        'errors_by_pid' => $errors_by_pid,
        'recent_errors' => array_slice($recent_errors, -10)  // Return last 10 only
    ]);
}
