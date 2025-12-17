<?php
/**
 * CariTranscoder - Inputs API
 * Copyright (c) 2024 CariTech Solutions
 */

define('CARITRANS', true);
require_once __DIR__ . '/../includes/config.php';
require_once __DIR__ . '/../includes/auth.php';
require_once __DIR__ . '/../includes/functions.php';

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

    default:
        json_response(['error' => 'Invalid action'], 400);
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
    $config_file = CONFIG_DIR . '/inputs/' . $id . '.ini';
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
    $config_file = CONFIG_DIR . '/inputs/' . $id . '.ini';

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

    // Try using tsp (TSDuck) first
    $tsduck_available = shell_exec('which tsp 2>/dev/null');

    if ($tsduck_available) {
        $result = scan_with_tsduck($scan_url, $type);
    } else {
        // Fallback to ffprobe
        $result = scan_with_ffprobe($scan_url);
    }

    return $result;
}

/**
 * Build source URL from parameters
 */
function build_source_url($source, $type) {
    // If source is already a full URL, return it
    if (preg_match('/^(udp|srt|rtmp|http|https):\/\//', $source)) {
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
        default:
            return $source;
    }
}

/**
 * Scan using TSDuck
 */
function scan_with_tsduck($url, $type) {
    $result = [
        'success' => false,
        'programs' => [],
        'video_pids' => [],
        'audio_pids' => [],
        'error' => null
    ];

    // Use tsp to analyze the stream for 3 seconds
    $cmd = '';

    if ($type === 'udp' || strpos($url, 'udp://') === 0) {
        // Parse UDP URL
        $parsed = parse_url(str_replace('udp://', 'http://', $url));
        $address = $parsed['host'] ?? '';
        $port = $parsed['port'] ?? 5000;

        $cmd = sprintf(
            'timeout 5 tsp -I ip %s:%d -P analyze -o /dev/stdout --normalized 2>/dev/null | head -100',
            escapeshellarg($address),
            (int)$port
        );
    } elseif ($type === 'srt' || strpos($url, 'srt://') === 0) {
        // SRT input
        $cmd = sprintf(
            'timeout 5 tsp -I srt %s -P analyze -o /dev/stdout --normalized 2>/dev/null | head -100',
            escapeshellarg($url)
        );
    } else {
        $result['error'] = 'Unsupported input type for TSDuck scanning';
        return $result;
    }

    exec($cmd, $output, $code);

    if ($code !== 0 && empty($output)) {
        $result['error'] = 'Failed to scan source (timeout or no data)';
        return $result;
    }

    // Parse TSDuck analyze output
    $output_text = implode("\n", $output);

    // Extract PIDs - looking for patterns like "pid=256" or "PID: 256"
    // Video PIDs typically have stream_type for video (0x1B for H.264, 0x24 for H.265)
    // Audio PIDs have stream_type for audio (0x0F for AAC, 0x03/0x04 for MPEG audio)

    // Simplified parsing - look for service/program info
    preg_match_all('/service.*?id[=:\s]+(\d+)/i', $output_text, $prog_matches);
    if (!empty($prog_matches[1])) {
        $result['programs'] = array_unique($prog_matches[1]);
    }

    // Look for video PIDs
    preg_match_all('/video.*?pid[=:\s]+(\d+)|pid[=:\s]+(\d+).*?video/i', $output_text, $vid_matches);
    $video_pids = array_filter(array_merge($vid_matches[1] ?? [], $vid_matches[2] ?? []));
    if (!empty($video_pids)) {
        foreach ($video_pids as $pid) {
            $result['video_pids'][] = [
                'pid' => (int)$pid,
                'codec' => 'H.264',
                'description' => 'Video PID ' . $pid
            ];
        }
    }

    // Look for audio PIDs
    preg_match_all('/audio.*?pid[=:\s]+(\d+)|pid[=:\s]+(\d+).*?audio/i', $output_text, $aud_matches);
    $audio_pids = array_filter(array_merge($aud_matches[1] ?? [], $aud_matches[2] ?? []));
    if (!empty($audio_pids)) {
        foreach ($audio_pids as $pid) {
            $result['audio_pids'][] = [
                'pid' => (int)$pid,
                'codec' => 'AAC',
                'language' => 'und',
                'description' => 'Audio PID ' . $pid
            ];
        }
    }

    $result['success'] = true;
    $result['raw_output'] = $output_text;

    return $result;
}

/**
 * Scan using ffprobe
 */
function scan_with_ffprobe($url) {
    $result = [
        'success' => false,
        'programs' => [],
        'video_pids' => [],
        'audio_pids' => [],
        'error' => null
    ];

    $cmd = sprintf(
        'timeout 10 ffprobe -v quiet -show_programs -show_streams -print_format json %s 2>/dev/null',
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
    $config = [
        'general' => [
            'name' => $name,
            'type' => $data['type'] ?? 'udp',
            'enabled' => 1,
            'buffer' => $data['buffer'] ?? 'buffer-input-' . $id
        ],
        'sources' => [],
        'pids' => [
            'video' => $data['video_pid'] ?? '',
            'audio' => is_array($data['audio_pids'] ?? null) ? implode(',', $data['audio_pids']) : ($data['audio_pids'] ?? ''),
            'program' => $data['program_pid'] ?? ''
        ]
    ];

    // Add sources
    $sources = $data['sources'] ?? [];
    if (!empty($sources)) {
        foreach ($sources as $idx => $source) {
            $url = $source['url'] ?? $source;
            $weight = $source['weight'] ?? 10;
            $config['sources']['source_' . $idx] = $url . '|' . $weight;
        }
    }

    // Add type-specific settings
    switch ($data['type'] ?? 'udp') {
        case 'udp':
            $config['udp'] = [
                'address' => $data['udp_address'] ?? '',
                'port' => $data['udp_port'] ?? 5000,
                'interface' => $data['udp_interface'] ?? ''
            ];
            break;
        case 'srt':
            $config['srt'] = [
                'mode' => $data['srt_mode'] ?? 'listener',
                'address' => $data['srt_address'] ?? '0.0.0.0',
                'port' => $data['srt_port'] ?? 9000,
                'latency' => $data['srt_latency'] ?? 200,
                'passphrase' => $data['srt_passphrase'] ?? ''
            ];
            break;
        case 'rtmp':
            $config['rtmp'] = [
                'url' => $data['rtmp_url'] ?? ''
            ];
            break;
        case 'hls':
            $config['hls'] = [
                'url' => $data['hls_url'] ?? ''
            ];
            break;
        case 'file':
            $config['file'] = [
                'path' => $data['file_path'] ?? '',
                'loop' => $data['file_loop'] ?? 1
            ];
            break;
    }

    // Write config file
    $config_file = $config_dir . '/' . $id . '.ini';
    $content = build_ini_content($config);

    if (file_put_contents($config_file, $content)) {
        return ['success' => true, 'id' => $id, 'message' => "Input '{$name}' created successfully"];
    }

    return ['success' => false, 'error' => 'Failed to save configuration'];
}

/**
 * Update an existing input
 */
function update_input($id, $data) {
    $id = preg_replace('/[^a-zA-Z0-9_-]/', '', $id);
    $config_file = CONFIG_DIR . '/inputs/' . $id . '.ini';

    if (!file_exists($config_file)) {
        return ['success' => false, 'error' => 'Input not found'];
    }

    // Load existing config
    $config = parse_ini_file($config_file, true);

    // Update fields
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

    // Update sources
    if (isset($data['sources'])) {
        $config['sources'] = [];
        foreach ($data['sources'] as $idx => $source) {
            $url = $source['url'] ?? $source;
            $weight = $source['weight'] ?? 10;
            $config['sources']['source_' . $idx] = $url . '|' . $weight;
        }
    }

    // Write updated config
    $content = build_ini_content($config);

    if (file_put_contents($config_file, $content)) {
        return ['success' => true, 'message' => "Input updated successfully"];
    }

    return ['success' => false, 'error' => 'Failed to save configuration'];
}

/**
 * Delete an input
 */
function delete_input($id) {
    $id = preg_replace('/[^a-zA-Z0-9_-]/', '', $id);

    // Stop service first
    stop_input_service($id);

    // Remove config file
    $config_file = CONFIG_DIR . '/inputs/' . $id . '.ini';

    if (file_exists($config_file)) {
        unlink($config_file);
    }

    return ['success' => true, 'message' => "Input deleted successfully"];
}

/**
 * Start input service
 */
function start_input_service($id) {
    $id = preg_replace('/[^a-zA-Z0-9_-]/', '', $id);

    exec("systemctl start cari-input@{$id} 2>&1", $output, $code);

    if ($code === 0) {
        return ['success' => true, 'message' => "Input service started"];
    }

    return ['success' => false, 'error' => implode("\n", $output)];
}

/**
 * Stop input service
 */
function stop_input_service($id) {
    $id = preg_replace('/[^a-zA-Z0-9_-]/', '', $id);

    exec("systemctl stop cari-input@{$id} 2>&1", $output, $code);

    if ($code === 0) {
        return ['success' => true, 'message' => "Input service stopped"];
    }

    return ['success' => false, 'error' => implode("\n", $output)];
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
