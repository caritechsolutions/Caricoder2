<?php
/**
 * CariTranscoder - Pipelines API
 * Copyright (c) 2024 CariTech Solutions
 */

define('CARITRANS', true);
require_once __DIR__ . '/../../includes/config.php';
require_once __DIR__ . '/../../includes/auth.php';
require_once __DIR__ . '/../../includes/functions.php';

// Require login
if (!auth_is_logged_in()) {
    json_response(['error' => 'Unauthorized'], 401);
}

$action = $_GET['action'] ?? 'list';

switch ($action) {
    case 'list':
        // Get all pipelines (combination of inputs, transcoders, muxers, outputs)
        $pipelines = get_all_pipelines();
        json_response(['pipelines' => $pipelines]);
        break;

    case 'stats':
        // Get pipeline statistics for bitrate display
        $pipelines = get_all_pipelines();
        $stats = [];

        foreach ($pipelines as $pipeline) {
            $stats[] = [
                'id' => $pipeline['id'],
                'name' => $pipeline['name'],
                'status' => $pipeline['status'],
                'input_bitrate' => $pipeline['input_bitrate'] ?? 0,
                'output_bitrate' => $pipeline['output_bitrate'] ?? 0,
            ];
        }

        json_response(['pipelines' => $stats]);
        break;

    case 'start':
        if (!isset($_POST['id'])) {
            json_response(['error' => 'Pipeline ID required'], 400);
        }
        $result = start_pipeline($_POST['id']);
        json_response($result);
        break;

    case 'stop':
        if (!isset($_POST['id'])) {
            json_response(['error' => 'Pipeline ID required'], 400);
        }
        $result = stop_pipeline($_POST['id']);
        json_response($result);
        break;

    case 'restart':
        if (!isset($_POST['id'])) {
            json_response(['error' => 'Pipeline ID required'], 400);
        }
        $result = restart_pipeline($_POST['id']);
        json_response($result);
        break;

    case 'create':
        // Create a new pipeline
        $data = json_decode(file_get_contents('php://input'), true);
        if (!$data) {
            json_response(['error' => 'Invalid JSON data'], 400);
        }
        $result = create_pipeline($data);
        json_response($result);
        break;

    case 'delete':
        if (!isset($_POST['id'])) {
            json_response(['error' => 'Pipeline ID required'], 400);
        }
        $result = delete_pipeline($_POST['id']);
        json_response($result);
        break;

    default:
        json_response(['error' => 'Invalid action'], 400);
}

/**
 * Get all active pipelines
 */
function get_all_pipelines() {
    $pipelines = [];

    // Load pipelines from config
    $config_file = CONFIG_DIR . '/pipelines.ini';
    if (file_exists($config_file)) {
        $config = parse_ini_file($config_file, true);
        foreach ($config as $id => $pipeline) {
            $pipelines[] = [
                'id' => $id,
                'name' => $pipeline['name'] ?? $id,
                'status' => check_pipeline_status($id),
                'input' => $pipeline['input'] ?? '',
                'processing' => $pipeline['processing'] ?? '',
                'output' => $pipeline['output'] ?? '',
                'input_bitrate' => get_pipeline_bitrate($id, 'input'),
                'output_bitrate' => get_pipeline_bitrate($id, 'output'),
            ];
        }
    }

    return $pipelines;
}

/**
 * Check pipeline status
 */
function check_pipeline_status($id) {
    // Check if systemd service is running
    $service = "cari-pipeline@{$id}";
    exec("systemctl is-active " . escapeshellarg($service) . " 2>/dev/null", $output, $code);

    if ($code === 0) {
        return 'running';
    }

    // Also check individual components
    $components = ['input', 'transcoder', 'mux', 'output'];
    foreach ($components as $comp) {
        $service = "cari-{$comp}@{$id}";
        exec("systemctl is-active " . escapeshellarg($service) . " 2>/dev/null", $output, $code);
        if ($code === 0) {
            return 'running';
        }
    }

    return 'stopped';
}

/**
 * Get pipeline bitrate
 */
function get_pipeline_bitrate($id, $type) {
    // Read from stats file if available
    $stats_file = "/var/run/caritrans/{$id}_{$type}_bitrate";
    if (file_exists($stats_file)) {
        return (int)file_get_contents($stats_file);
    }
    return 0;
}

/**
 * Start a pipeline
 */
function start_pipeline($id) {
    // Sanitize ID
    $id = preg_replace('/[^a-zA-Z0-9_-]/', '', $id);

    // Start the pipeline service
    exec("systemctl start cari-pipeline@{$id} 2>&1", $output, $code);

    if ($code === 0) {
        return ['success' => true, 'message' => "Pipeline {$id} started"];
    }

    return ['success' => false, 'error' => implode("\n", $output)];
}

/**
 * Stop a pipeline
 */
function stop_pipeline($id) {
    $id = preg_replace('/[^a-zA-Z0-9_-]/', '', $id);

    exec("systemctl stop cari-pipeline@{$id} 2>&1", $output, $code);

    if ($code === 0) {
        return ['success' => true, 'message' => "Pipeline {$id} stopped"];
    }

    return ['success' => false, 'error' => implode("\n", $output)];
}

/**
 * Restart a pipeline
 */
function restart_pipeline($id) {
    stop_pipeline($id);
    usleep(500000); // 500ms delay
    return start_pipeline($id);
}

/**
 * Create a new pipeline
 */
function create_pipeline($data) {
    if (empty($data['name'])) {
        return ['success' => false, 'error' => 'Pipeline name required'];
    }

    $id = preg_replace('/[^a-zA-Z0-9_-]/', '', strtolower($data['name']));
    $config_file = CONFIG_DIR . '/pipelines.ini';

    // Load existing config
    $config = [];
    if (file_exists($config_file)) {
        $config = parse_ini_file($config_file, true);
    }

    // Add new pipeline
    $config[$id] = [
        'name' => $data['name'],
        'input' => $data['input'] ?? '',
        'processing' => $data['processing'] ?? '',
        'output' => $data['output'] ?? '',
        'enabled' => 1,
    ];

    // Write config
    $content = "";
    foreach ($config as $section => $values) {
        $content .= "[{$section}]\n";
        foreach ($values as $key => $val) {
            $content .= "{$key} = {$val}\n";
        }
        $content .= "\n";
    }

    if (file_put_contents($config_file, $content)) {
        return ['success' => true, 'id' => $id, 'message' => "Pipeline {$id} created"];
    }

    return ['success' => false, 'error' => 'Failed to save pipeline configuration'];
}

/**
 * Delete a pipeline
 */
function delete_pipeline($id) {
    $id = preg_replace('/[^a-zA-Z0-9_-]/', '', $id);

    // Stop first
    stop_pipeline($id);

    // Remove from config
    $config_file = CONFIG_DIR . '/pipelines.ini';
    if (file_exists($config_file)) {
        $config = parse_ini_file($config_file, true);
        unset($config[$id]);

        $content = "";
        foreach ($config as $section => $values) {
            $content .= "[{$section}]\n";
            foreach ($values as $key => $val) {
                $content .= "{$key} = {$val}\n";
            }
            $content .= "\n";
        }

        file_put_contents($config_file, $content);
    }

    return ['success' => true, 'message' => "Pipeline {$id} deleted"];
}
