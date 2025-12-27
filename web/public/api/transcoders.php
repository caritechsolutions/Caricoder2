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
    case 'inputs':
        handle_get_inputs();
        break;
    case 'next_id':
        handle_get_next_id();
        break;
    case 'all_metrics':
        handle_all_metrics();
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
        $files = glob($transcoders_dir . '/' . $base_id . '_trans_*.conf');
        foreach ($files as $file) {
            $filename = basename($file, '.conf');
            if (preg_match('/_trans_(\d+)$/', $filename, $matches)) {
                $num = intval($matches[1]);
                if ($num > $max_num) {
                    $max_num = $num;
                }
            }
        }
    }

    $next_num = $max_num + 1;
    $next_id = $base_id . '_trans_' . $next_num;
    $next_name = $base_id . '_trans_' . $next_num;

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

    $id = preg_replace('/[^a-z0-9-]/', '', strtolower($input['transcoder_id']));
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
 * Get metrics for a single transcoder
 * Parses the last bitrate_monitor output from the log file
 * Also fetches input bitrate from linked input service
 */
function get_transcoder_metrics($id) {
    $log_file = "/var/log/caritrans/transcoder-{$id}.log";
    $config_file = CONFIG_PATH . '/transcoders/' . $id . '.conf';

    $metrics = [
        'status' => 'offline',
        'output_video_bitrate' => 0,
        'output_audio_bitrate' => 0,
        'input_video_bitrate' => 0,
        'input_audio_bitrate' => 0,
        'input_format' => null,
        'output_format' => null
    ];

    // Load transcoder config for input source and format info
    if (file_exists($config_file)) {
        $config = parse_config($config_file);

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

            // Fetch input metrics from the input's API
            $input_config_file = CONFIG_PATH . '/inputs/' . $source_service . '.conf';
            if (file_exists($input_config_file)) {
                $input_config = parse_config($input_config_file);
                $api_port = $input_config['output']['api_port'] ?? null;

                if ($api_port) {
                    // Query the input's API for metrics
                    $url = "http://127.0.0.1:{$api_port}/metrics";
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
        // Format: * bitrate_monitor: YYYY/MM/DD HH:MM:SS, PID 0x0041 (65) bitrate: 1234567 bits/s
        // Since GStreamer mpegtsmux auto-assigns PIDs, we detect video/audio by bitrate magnitude:
        // - Video: typically > 500,000 bits/s (highest bitrate stream)
        // - Audio: typically 32,000 - 500,000 bits/s (second highest)
        // - Other PIDs (PAT, PMT, etc.): very low bitrate, ignored

        $pid_bitrates = [];

        foreach (array_reverse($lines) as $line) {
            if (strpos($line, 'bitrate_monitor') !== false) {
                if (preg_match('/PID\s+0x[0-9a-fA-F]+\s+\((\d+)\)\s+bitrate:\s+(\d+)\s+bits\/s/', $line, $matches)) {
                    $pid = intval($matches[1]);
                    $bitrate = intval($matches[2]);

                    // Only track PIDs we haven't seen yet (most recent value)
                    // Skip very low bitrate PIDs (PAT, PMT, etc. are typically < 10000 bps)
                    if (!isset($pid_bitrates[$pid]) && $bitrate > 10000) {
                        $pid_bitrates[$pid] = $bitrate;
                    }
                }
            }
        }

        // Sort by bitrate descending and classify
        arsort($pid_bitrates);
        $sorted_bitrates = array_values($pid_bitrates);

        // Highest bitrate is video, second highest is audio
        if (count($sorted_bitrates) >= 1) {
            $metrics['output_video_bitrate'] = $sorted_bitrates[0];
        }
        if (count($sorted_bitrates) >= 2) {
            $metrics['output_audio_bitrate'] = $sorted_bitrates[1];
        }
    }

    // For backwards compatibility, also set video_bitrate/audio_bitrate
    $metrics['video_bitrate'] = $metrics['output_video_bitrate'];
    $metrics['audio_bitrate'] = $metrics['output_audio_bitrate'];

    return $metrics;
}
