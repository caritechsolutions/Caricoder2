<?php
/**
 * CariTranscoder - Authentication System
 * Copyright (c) 2024 CariTech Solutions
 */

if (!defined('CARITRANS')) {
    die('Direct access not permitted');
}

/**
 * User roles and permissions
 */
define('ROLE_ADMIN', 'admin');
define('ROLE_OPERATOR', 'operator');
define('ROLE_VIEWER', 'viewer');

$ROLE_PERMISSIONS = [
    ROLE_ADMIN => ['view', 'create', 'edit', 'delete', 'admin', 'system'],
    ROLE_OPERATOR => ['view', 'create', 'edit', 'delete'],
    ROLE_VIEWER => ['view']
];

/**
 * Initialize session
 */
function auth_init() {
    if (session_status() === PHP_SESSION_NONE) {
        session_name(SESSION_NAME);
        session_start();
    }

    // Regenerate session ID periodically
    if (!isset($_SESSION['created'])) {
        $_SESSION['created'] = time();
    } else if (time() - $_SESSION['created'] > 1800) {
        session_regenerate_id(true);
        $_SESSION['created'] = time();
    }

    // Check session timeout
    if (isset($_SESSION['last_activity'])) {
        if (time() - $_SESSION['last_activity'] > SESSION_TIMEOUT) {
            auth_logout();
            return false;
        }
    }
    $_SESSION['last_activity'] = time();

    return true;
}

/**
 * Load users from config file
 */
function auth_load_users() {
    $users = [];

    if (!file_exists(USERS_CONFIG)) {
        // Create default admin user
        $default_hash = hash('sha256', PASSWORD_SALT . ':admin');
        $users['admin'] = [
            'password_hash' => $default_hash,
            'role' => ROLE_ADMIN,
            'enabled' => true
        ];
        return $users;
    }

    $lines = file(USERS_CONFIG, FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES);
    foreach ($lines as $line) {
        $line = trim($line);
        if (empty($line) || $line[0] === '#') continue;

        $parts = explode(':', $line);
        if (count($parts) >= 4) {
            $username = $parts[0];
            $users[$username] = [
                'password_hash' => $parts[1],
                'role' => $parts[2],
                'enabled' => strtolower($parts[3]) === 'true'
            ];
        }
    }

    return $users;
}

/**
 * Verify password
 */
function auth_verify_password($password, $stored_hash) {
    // Handle our custom hash format
    if (strpos($stored_hash, '$sha256$') === 0) {
        $parts = explode('$', $stored_hash);
        if (count($parts) >= 4) {
            $salt = $parts[2];
            $hash = $parts[3];
            $computed = hash('sha256', $salt . ':' . $password);
            return hash_equals($hash, $computed);
        }
    }

    // Simple salted hash
    $computed = hash('sha256', PASSWORD_SALT . ':' . $password);
    return hash_equals($stored_hash, $computed);
}

/**
 * Hash password for storage
 */
function auth_hash_password($password) {
    $hash = hash('sha256', PASSWORD_SALT . ':' . $password);
    return '$sha256$' . PASSWORD_SALT . '$' . $hash;
}

/**
 * Attempt login
 */
function auth_login($username, $password) {
    $users = auth_load_users();

    if (!isset($users[$username])) {
        error_log("Login failed: unknown user '$username'");
        return false;
    }

    $user = $users[$username];

    if (!$user['enabled']) {
        error_log("Login failed: user '$username' is disabled");
        return false;
    }

    if (!auth_verify_password($password, $user['password_hash'])) {
        error_log("Login failed: invalid password for '$username'");
        return false;
    }

    // Success - create session
    $_SESSION['user'] = $username;
    $_SESSION['role'] = $user['role'];
    $_SESSION['login_time'] = time();
    $_SESSION['ip'] = $_SERVER['REMOTE_ADDR'];

    // Generate CSRF token
    $_SESSION[CSRF_TOKEN_NAME] = bin2hex(random_bytes(32));

    error_log("Login successful: '$username' from " . $_SERVER['REMOTE_ADDR']);
    return true;
}

/**
 * Logout
 */
function auth_logout() {
    $_SESSION = [];

    if (ini_get("session.use_cookies")) {
        $params = session_get_cookie_params();
        setcookie(session_name(), '', time() - 42000,
            $params["path"], $params["domain"],
            $params["secure"], $params["httponly"]
        );
    }

    session_destroy();
}

/**
 * Check if user is logged in
 */
function auth_is_logged_in() {
    return isset($_SESSION['user']) && !empty($_SESSION['user']);
}

/**
 * Get current user
 */
function auth_get_user() {
    return $_SESSION['user'] ?? null;
}

/**
 * Get current user role
 */
function auth_get_role() {
    return $_SESSION['role'] ?? null;
}

/**
 * Check if user has permission
 */
function auth_has_permission($permission) {
    global $ROLE_PERMISSIONS;

    $role = auth_get_role();
    if (!$role || !isset($ROLE_PERMISSIONS[$role])) {
        return false;
    }

    return in_array($permission, $ROLE_PERMISSIONS[$role]);
}

/**
 * Require login - redirect if not logged in
 */
function auth_require_login() {
    if (!auth_is_logged_in()) {
        header('Location: login.php');
        exit;
    }
}

/**
 * Require permission
 */
function auth_require_permission($permission) {
    auth_require_login();

    if (!auth_has_permission($permission)) {
        http_response_code(403);
        die('Permission denied');
    }
}

/**
 * Get CSRF token
 */
function auth_get_csrf_token() {
    if (!isset($_SESSION[CSRF_TOKEN_NAME])) {
        $_SESSION[CSRF_TOKEN_NAME] = bin2hex(random_bytes(32));
    }
    return $_SESSION[CSRF_TOKEN_NAME];
}

/**
 * Verify CSRF token
 */
function auth_verify_csrf($token) {
    if (!isset($_SESSION[CSRF_TOKEN_NAME])) {
        return false;
    }
    return hash_equals($_SESSION[CSRF_TOKEN_NAME], $token);
}

/**
 * CSRF hidden input field
 */
function auth_csrf_field() {
    return '<input type="hidden" name="' . CSRF_TOKEN_NAME . '" value="' . htmlspecialchars(auth_get_csrf_token()) . '">';
}

/**
 * Change user password
 */
function auth_change_password($username, $new_password) {
    if (!file_exists(USERS_CONFIG)) {
        return false;
    }

    $lines = file(USERS_CONFIG, FILE_IGNORE_NEW_LINES);
    $found = false;

    foreach ($lines as &$line) {
        if (empty(trim($line)) || $line[0] === '#') continue;

        $parts = explode(':', $line);
        if (count($parts) >= 4 && $parts[0] === $username) {
            $parts[1] = auth_hash_password($new_password);
            $line = implode(':', $parts);
            $found = true;
            break;
        }
    }

    if ($found) {
        file_put_contents(USERS_CONFIG, implode("\n", $lines));
        return true;
    }

    return false;
}

// Initialize auth on include
auth_init();
