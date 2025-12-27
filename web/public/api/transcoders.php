<?php
/**
 * CariTranscoder - Transcoders API
 * Copyright (c) 2024 CariTech Solutions
 */

define('CARITRANS', true);
require_once __DIR__ . '/../../includes/config.php';
require_once __DIR__ . '/../../includes/auth.php';
require_once __DIR__ . '/../../includes/functions.php';

// Check authentication
if (!auth_check()) {
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
    case 'list':
    default:
        handle_list();
        break;
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
    $config_file = CONFIG_DIR . '/transcoders/' . $id . '.conf';

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

    $id = preg_replace('/[^a-z0-9-]/', '', strtolower($input['transcoder_id']));
    if (empty($id)) {
        echo json_encode(['success' => false, 'error' => 'Invalid ID']);
        return;
    }

    // Check if already exists
    $config_file = CONFIG_DIR . '/transcoders/' . $id . '.conf';
    if (file_exists($config_file)) {
        echo json_encode(['success' => false, 'error' => 'Transcoder ID already exists']);
        return;
    }

    // Build config
    $config = build_transcoder_config($input);

    // Ensure directory exists
    if (!is_dir(CONFIG_DIR . '/transcoders')) {
        mkdir(CONFIG_DIR . '/transcoders', 0755, true);
    }

    // Write config file
    if (!write_ini_file($config_file, $config)) {
        echo json_encode(['success' => false, 'error' => 'Failed to write config file']);
        return;
    }

    // Create systemd service
    $result = create_transcoder_service($id, $config);
    if (!$result['success']) {
        // Clean up config file on failure
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
    $config_file = CONFIG_DIR . '/transcoders/' . $id . '.conf';

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

    // Stop existing service
    stop_transcoder_service($id);

    // Recreate systemd service
    $result = create_transcoder_service($id, $config);
    if (!$result['success']) {
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
    $config_file = CONFIG_DIR . '/transcoders/' . $id . '.conf';

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
    $config = [
        'general' => [
            'name' => $input['name'] ?? '',
            'enabled' => true
        ],
        'input' => [
            'address' => $input['input_address'] ?? '',
            'port' => intval($input['input_port'] ?? 5000)
        ],
        'output' => [
            'address' => $input['output_address'] ?? '',
            'port' => intval($input['output_port'] ?? 5000),
            'api_port' => intval($input['api_port'] ?? 9200)
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
            'samplerate' => intval($input['audio_samplerate'] ?? 48000),
            'aac_coder' => $input['aac_coder'] ?? 'fast',
            'aac_is' => !empty($input['aac_is']),
            'aac_ms' => !empty($input['aac_ms']),
            'aac_pns' => !empty($input['aac_pns']),
            'aac_tns' => !empty($input['aac_tns']),
            'aac_ltp' => !empty($input['aac_ltp']),
            'aac_pred' => !empty($input['aac_pred']),
            'aac_cutoff' => intval($input['aac_cutoff'] ?? 0),
            'aac_strict' => intval($input['aac_strict'] ?? 0)
        ]
    ];

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

    // Calculate tsp bitrate (video + audio + 5% overhead)
    $total_bitrate = $video['bitrate'] + $audio['bitrate'];
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
        'aac_coder' => $audio['aac_coder'],
        'aac_is' => $audio['aac_is'],
        'aac_ms' => $audio['aac_ms'],
        'aac_pns' => $audio['aac_pns'],
        'aac_tns' => $audio['aac_tns'],
        'aac_ltp' => $audio['aac_ltp'],
        'aac_pred' => $audio['aac_pred'],
        'aac_cutoff' => $audio['aac_cutoff'],
        'aac_strict' => $audio['aac_strict']
    ];

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
