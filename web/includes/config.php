<?php
/**
 * CariTranscoder - Configuration
 * Copyright (c) 2024 CariTech Solutions
 */

// Prevent direct access
if (!defined('CARITRANS')) {
    die('Direct access not permitted');
}

// Version
define('CARITRANS_VERSION', '1.0.1');

// Paths
define('CONFIG_DIR', '/etc/caritrans');
define('RUN_DIR', '/run/caritrans');
define('LOG_DIR', '/var/log/caritrans');
define('DATA_DIR', '/var/lib/caritrans');

// For development, use local paths
if (!is_dir(CONFIG_DIR)) {
    define('DEV_MODE', true);
    define('CONFIG_PATH', dirname(__DIR__, 2) . '/config');
} else {
    define('DEV_MODE', false);
    define('CONFIG_PATH', CONFIG_DIR);
}

// Main config file
define('MAIN_CONFIG', CONFIG_PATH . '/caritrans.conf');
define('USERS_CONFIG', CONFIG_PATH . '/users.conf');
define('LICENSE_FILE', CONFIG_PATH . '/license.key');

// Session settings
define('SESSION_NAME', 'caritrans_session');
define('SESSION_TIMEOUT', 3600); // 1 hour

// Security
define('CSRF_TOKEN_NAME', 'csrf_token');
define('PASSWORD_SALT', 'caritrans');

// WebSocket settings (for real-time stats)
define('STATS_WS_PORT', 8081);

// API settings
define('API_RATE_LIMIT', 100); // requests per minute

/**
 * Parse INI-style config file
 */
function parse_config($filename) {
    if (!file_exists($filename)) {
        return [];
    }

    $config = [];
    $current_section = '';

    $lines = file($filename, FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES);
    foreach ($lines as $line) {
        $line = trim($line);

        // Skip comments
        if ($line[0] === '#' || $line[0] === ';') {
            continue;
        }

        // Section header
        if ($line[0] === '[' && substr($line, -1) === ']') {
            $current_section = trim(substr($line, 1, -1));
            if (!isset($config[$current_section])) {
                $config[$current_section] = [];
            }
            continue;
        }

        // Key = value
        $pos = strpos($line, '=');
        if ($pos !== false) {
            $key = trim(substr($line, 0, $pos));
            $value = trim(substr($line, $pos + 1));

            // Remove quotes
            if ((substr($value, 0, 1) === '"' && substr($value, -1) === '"') ||
                (substr($value, 0, 1) === "'" && substr($value, -1) === "'")) {
                $value = substr($value, 1, -1);
            }

            if ($current_section) {
                $config[$current_section][$key] = $value;
            } else {
                $config[$key] = $value;
            }
        }
    }

    return $config;
}

/**
 * Get config value with default
 */
function config_get($config, $section, $key, $default = null) {
    if (isset($config[$section][$key])) {
        return $config[$section][$key];
    }
    return $default;
}

/**
 * Convert config value to boolean
 */
function config_bool($value) {
    if (is_bool($value)) return $value;
    $value = strtolower(trim($value));
    return in_array($value, ['true', 'yes', 'on', '1']);
}

// Load main configuration
$GLOBALS['main_config'] = parse_config(MAIN_CONFIG);
