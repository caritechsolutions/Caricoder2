<?php
/**
 * CariTranscoder - Helper Functions
 * Copyright (c) 2024 CariTech Solutions
 */

if (!defined('CARITRANS')) {
    die('Direct access not permitted');
}

/**
 * Format bytes to human readable
 */
function format_bytes($bytes, $precision = 2) {
    $units = ['B', 'KB', 'MB', 'GB', 'TB'];
    $bytes = max($bytes, 0);
    $pow = floor(($bytes ? log($bytes) : 0) / log(1024));
    $pow = min($pow, count($units) - 1);
    $bytes /= pow(1024, $pow);
    return round($bytes, $precision) . ' ' . $units[$pow];
}

/**
 * Format bitrate to human readable
 */
function format_bitrate($bps) {
    if ($bps >= 1000000000) {
        return round($bps / 1000000000, 2) . ' Gbps';
    } else if ($bps >= 1000000) {
        return round($bps / 1000000, 2) . ' Mbps';
    } else if ($bps >= 1000) {
        return round($bps / 1000, 2) . ' Kbps';
    }
    return $bps . ' bps';
}

/**
 * Get system statistics
 */
function get_system_stats() {
    $stats = [
        'cpu_percent' => 0,
        'mem_percent' => 0,
        'mem_used' => 0,
        'mem_total' => 0,
        'load_avg' => [0, 0, 0],
        'net_rx' => 0,
        'net_tx' => 0,
        'uptime' => 0,
        'cluster_enabled' => false,
        'cluster_nodes' => []
    ];

    // CPU usage (simplified)
    if (file_exists('/proc/stat')) {
        $stat = file_get_contents('/proc/stat');
        if (preg_match('/^cpu\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)/m', $stat, $m)) {
            $total = $m[1] + $m[2] + $m[3] + $m[4];
            $idle = $m[4];
            $stats['cpu_percent'] = $total > 0 ? round((($total - $idle) / $total) * 100) : 0;
        }
    }

    // Load average
    if (file_exists('/proc/loadavg')) {
        $load = explode(' ', file_get_contents('/proc/loadavg'));
        $stats['load_avg'] = [
            round((float)$load[0], 2),
            round((float)$load[1], 2),
            round((float)$load[2], 2)
        ];
    }

    // Memory usage
    if (file_exists('/proc/meminfo')) {
        $meminfo = file_get_contents('/proc/meminfo');
        if (preg_match('/MemTotal:\s+(\d+)/', $meminfo, $m)) {
            $stats['mem_total'] = $m[1] * 1024;
        }
        if (preg_match('/MemAvailable:\s+(\d+)/', $meminfo, $m)) {
            $stats['mem_used'] = $stats['mem_total'] - ($m[1] * 1024);
        }
        if ($stats['mem_total'] > 0) {
            $stats['mem_percent'] = round(($stats['mem_used'] / $stats['mem_total']) * 100);
        }
    }

    // Network stats (simplified)
    if (file_exists('/proc/net/dev')) {
        $dev = file_get_contents('/proc/net/dev');
        $lines = explode("\n", $dev);
        foreach ($lines as $line) {
            if (strpos($line, 'eth0') !== false || strpos($line, 'ens') !== false) {
                $parts = preg_split('/\s+/', trim($line));
                if (count($parts) >= 10) {
                    $stats['net_rx'] = (int)$parts[1] * 8; // Convert to bits
                    $stats['net_tx'] = (int)$parts[9] * 8;
                }
                break;
            }
        }
    }

    // Check cluster config
    $config = $GLOBALS['main_config'] ?? [];
    $stats['cluster_enabled'] = config_bool(config_get($config, 'cluster', 'enabled', 'false'));

    if ($stats['cluster_enabled']) {
        // Add current node
        $stats['cluster_nodes'][] = [
            'name' => config_get($config, 'system', 'hostname', gethostname()),
            'role' => config_get($config, 'cluster', 'role', 'primary'),
            'status' => 'online',
            'load' => $stats['cpu_percent']
        ];

        // TODO: Get peer node status via cluster communication
    }

    return $stats;
}

/**
 * Get service list from config directory
 */
function get_service_list($type) {
    $dir = CONFIG_PATH . '/' . $type;
    $services = [];

    if (!is_dir($dir)) {
        return $services;
    }

    foreach (glob($dir . '/*.conf') as $file) {
        $config = parse_config($file);
        $section = rtrim($type, 's'); // inputs -> input

        // Try 'general' section first (new format), then fall back to singular section name
        $id = basename($file, '.conf');
        $name = config_get($config, 'general', 'name', null);
        if ($name === null) {
            $name = config_get($config, $section, 'name', $id);
        }

        $enabled_val = config_get($config, 'general', 'enabled', null);
        if ($enabled_val === null) {
            $enabled_val = config_get($config, $section, 'enabled', 'true');
        }
        $enabled = config_bool($enabled_val);

        // Get type from config
        $input_type = config_get($config, 'general', 'type', null);
        if ($input_type === null) {
            $input_type = config_get($config, $section, 'type', 'udp');
        }

        // Get source from sources section
        $source = 'Not configured';
        if (isset($config['sources'])) {
            foreach ($config['sources'] as $key => $value) {
                if (strpos($key, 'source_') === 0) {
                    // Format: type|url|weight|extra
                    $parts = explode('|', $value);
                    if (count($parts) >= 2) {
                        $source = $parts[1]; // URL is second part
                    } else {
                        $source = $parts[0]; // Fallback to first part
                    }
                    break; // Just show primary source
                }
            }
        }

        // Get service status from systemd
        $status = 'stopped';
        $output = [];
        // UDP inputs use cari-udp-{id} format, SRT inputs use cari-srt-{id}, HLS inputs use cari-hls-{id}, HTTP inputs use cari-http-{id}, RIST inputs use cari-rist-{id}, others use cari-{section}@{id}
        if ($section === 'input' && $input_type === 'udp') {
            $service_name = "cari-udp-{$id}";
        } elseif ($section === 'input' && $input_type === 'srt') {
            $service_name = "cari-srt-{$id}";
        } elseif ($section === 'input' && $input_type === 'hls') {
            $service_name = "cari-hls-{$id}";
        } elseif ($section === 'input' && $input_type === 'http') {
            $service_name = "cari-http-{$id}";
        } elseif ($section === 'input' && $input_type === 'rist') {
            $service_name = "cari-rist-{$id}";
        } else {
            $service_name = "cari-{$section}@{$id}";
        }
        exec("systemctl is-active {$service_name} 2>/dev/null", $output, $ret);
        if ($ret === 0) {
            $status = 'running';
        }

        $services[] = [
            'id' => $id,
            'name' => $name,
            'type' => $input_type,
            'source' => $source,
            'enabled' => $enabled,
            'status' => $status,
            'config' => $config,
            'config_file' => $file
        ];
    }

    return $services;
}

/**
 * Get active pipelines (services that are linked together)
 */
function get_active_pipelines() {
    $pipelines = [];

    // Get all inputs and trace their paths
    $inputs = get_service_list('inputs');

    foreach ($inputs as $input) {
        $pipeline = [
            'id' => $input['id'],
            'name' => $input['name'],
            'input_type' => config_get($input['config'], 'input', 'type', 'unknown'),
            'input_source' => get_input_source_display($input['config']),
            'transcode_enabled' => false,
            'video_codec' => 'passthrough',
            'output_type' => 'none',
            'output_dest' => '',
            'bitrate' => 0,
            'status' => $input['status']
        ];

        // Find connected transcoder
        $targets = config_get($input['config'], 'output', 'targets', '');
        if ($targets) {
            $target_id = explode(',', $targets)[0];
            $trans_file = CONFIG_PATH . '/transcoders/' . trim($target_id) . '.conf';
            if (file_exists($trans_file)) {
                $trans_config = parse_config($trans_file);
                $pipeline['transcode_enabled'] = config_get($trans_config, 'video', 'mode', 'passthrough') === 'transcode';
                $pipeline['video_codec'] = config_get($trans_config, 'video', 'codec', 'passthrough');

                // Find output from transcoder
                $out_targets = config_get($trans_config, 'output', 'targets', '');
                if ($out_targets) {
                    $out_id = explode(',', $out_targets)[0];
                    $out_file = CONFIG_PATH . '/outputs/' . trim($out_id) . '.conf';
                    if (file_exists($out_file)) {
                        $out_config = parse_config($out_file);
                        $pipeline['output_type'] = config_get($out_config, 'output', 'type', 'unknown');
                        $pipeline['output_dest'] = get_output_dest_display($out_config);
                    }
                }
            }
        }

        $pipelines[] = $pipeline;
    }

    return $pipelines;
}

/**
 * Get display string for input source
 */
function get_input_source_display($config) {
    $type = config_get($config, 'input', 'type', 'unknown');

    switch ($type) {
        case 'srt':
            $mode = config_get($config, 'source', 'mode', 'listener');
            if ($mode === 'listener') {
                return 'srt://0.0.0.0:' . config_get($config, 'source', 'listen_port', '4900');
            } else {
                return 'srt://' . config_get($config, 'source', 'remote_address', '') . ':' . config_get($config, 'source', 'remote_port', '4900');
            }
        case 'udp':
            return 'udp://' . config_get($config, 'source', 'address', '') . ':' . config_get($config, 'source', 'port', '');
        case 'rtmp':
            return config_get($config, 'source', 'url', 'rtmp://...');
        default:
            return $type;
    }
}

/**
 * Get display string for output destination
 */
function get_output_dest_display($config) {
    $type = config_get($config, 'output', 'type', 'unknown');

    switch ($type) {
        case 'udp':
            return 'udp://' . config_get($config, 'destination', 'address', '') . ':' . config_get($config, 'destination', 'port', '');
        case 'srt':
            $mode = config_get($config, 'destination_srt', 'mode', 'caller');
            if ($mode === 'listener') {
                return 'srt://0.0.0.0:' . config_get($config, 'destination_srt', 'listen_port', '4900');
            } else {
                return 'srt://' . config_get($config, 'destination_srt', 'remote_address', '') . ':' . config_get($config, 'destination_srt', 'remote_port', '4900');
            }
        case 'rtmp':
            return config_get($config, 'destination_rtmp', 'url', 'rtmp://...');
        default:
            return $type;
    }
}

/**
 * Get license information
 */
function get_license_info() {
    $license = [
        'type' => 'free',
        'status' => 'valid',
        'customer' => 'Unlicensed',
        'expiry' => 'N/A',
        'max_inputs' => 2,
        'max_transcoders' => 1,
        'max_outputs' => 2,
        'max_muxers' => 1,
        'max_nodes' => 1
    ];

    if (file_exists(LICENSE_FILE)) {
        $lic_config = parse_config(LICENSE_FILE);

        $license['type'] = config_get($lic_config, 'license', 'type', 'free');
        $license['customer'] = config_get($lic_config, 'license', 'customer', 'Unknown');

        $expiry = config_get($lic_config, 'license', 'expiry', 'perpetual');
        if ($expiry === 'perpetual') {
            $license['expiry'] = 'Never';
        } else {
            $exp_time = (int)$expiry;
            if ($exp_time > time()) {
                $license['expiry'] = date('Y-m-d', $exp_time);
            } else {
                $license['status'] = 'expired';
                $license['expiry'] = 'Expired';
            }
        }

        $license['max_inputs'] = (int)config_get($lic_config, 'limits', 'inputs', 0);
        $license['max_transcoders'] = (int)config_get($lic_config, 'limits', 'transcoders', 0);
        $license['max_outputs'] = (int)config_get($lic_config, 'limits', 'outputs', 0);
        $license['max_muxers'] = (int)config_get($lic_config, 'limits', 'muxers', 0);
        $license['max_nodes'] = (int)config_get($lic_config, 'limits', 'nodes', 1);
    }

    return $license;
}

/**
 * Start a service
 */
function service_start($type, $id) {
    if (!auth_has_permission('edit')) {
        return ['success' => false, 'message' => 'Permission denied'];
    }

    $service = "cari-{$type}@{$id}";
    exec("sudo /bin/systemctl start {$service} 2>&1", $output, $ret);

    return [
        'success' => $ret === 0,
        'message' => $ret === 0 ? 'Service started' : implode("\n", $output)
    ];
}

/**
 * Stop a service
 */
function service_stop($type, $id) {
    if (!auth_has_permission('edit')) {
        return ['success' => false, 'message' => 'Permission denied'];
    }

    $service = "cari-{$type}@{$id}";
    exec("sudo /bin/systemctl stop {$service} 2>&1", $output, $ret);

    return [
        'success' => $ret === 0,
        'message' => $ret === 0 ? 'Service stopped' : implode("\n", $output)
    ];
}

/**
 * Restart a service
 */
function service_restart($type, $id) {
    if (!auth_has_permission('edit')) {
        return ['success' => false, 'message' => 'Permission denied'];
    }

    $service = "cari-{$type}@{$id}";
    exec("sudo /bin/systemctl restart {$service} 2>&1", $output, $ret);

    return [
        'success' => $ret === 0,
        'message' => $ret === 0 ? 'Service restarted' : implode("\n", $output)
    ];
}

/**
 * Get service status
 */
function service_status($type, $id) {
    $service = "cari-{$type}@{$id}";

    exec("systemctl is-active {$service} 2>/dev/null", $active_out, $active_ret);
    exec("systemctl show {$service} --property=MainPID,MemoryCurrent,CPUUsageNSec 2>/dev/null", $show_out);

    $status = [
        'running' => $active_ret === 0,
        'state' => $active_ret === 0 ? 'running' : 'stopped',
        'pid' => 0,
        'memory' => 0,
        'cpu' => 0
    ];

    foreach ($show_out as $line) {
        list($key, $value) = explode('=', $line, 2);
        switch ($key) {
            case 'MainPID':
                $status['pid'] = (int)$value;
                break;
            case 'MemoryCurrent':
                $status['memory'] = (int)$value;
                break;
            case 'CPUUsageNSec':
                $status['cpu'] = (int)$value;
                break;
        }
    }

    return $status;
}

/**
 * Save configuration file
 */
function save_config($filename, $config) {
    $content = "# CariTranscoder Configuration\n";
    $content .= "# Generated: " . date('Y-m-d H:i:s') . "\n\n";

    $current_section = '';
    foreach ($config as $section => $values) {
        if (is_array($values)) {
            $content .= "[$section]\n";
            foreach ($values as $key => $value) {
                if (is_bool($value)) {
                    $value = $value ? 'true' : 'false';
                }
                $content .= "$key = $value\n";
            }
            $content .= "\n";
        }
    }

    return file_put_contents($filename, $content) !== false;
}

/**
 * JSON response helper
 */
function json_response($data, $status = 200) {
    http_response_code($status);
    header('Content-Type: application/json');
    echo json_encode($data);
    exit;
}

/**
 * Validate required fields
 */
function validate_required($data, $fields) {
    $missing = [];
    foreach ($fields as $field) {
        if (!isset($data[$field]) || $data[$field] === '') {
            $missing[] = $field;
        }
    }
    return $missing;
}

/**
 * Sanitize a name to create a valid ID
 * Converts to lowercase, replaces non-alphanumeric chars with hyphens
 */
function sanitize_name_to_id($name, $prefix = 'item') {
    $id = strtolower(trim($name));
    $id = preg_replace('/[^a-z0-9]+/', '-', $id);
    $id = trim($id, '-');
    return $id ?: $prefix . '-' . time();
}
