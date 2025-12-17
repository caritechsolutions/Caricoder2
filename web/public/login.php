<?php
/**
 * CariTranscoder - Login Page
 * Copyright (c) 2024 CariTech Solutions
 */

define('CARITRANS', true);
require_once __DIR__ . '/../includes/config.php';
require_once __DIR__ . '/../includes/auth.php';

// Redirect if already logged in
if (auth_is_logged_in()) {
    header('Location: index.php');
    exit;
}

$error = '';
$username = '';

// Handle login form submission
if ($_SERVER['REQUEST_METHOD'] === 'POST') {
    $username = trim($_POST['username'] ?? '');
    $password = $_POST['password'] ?? '';

    if (empty($username) || empty($password)) {
        $error = 'Please enter username and password';
    } else if (auth_login($username, $password)) {
        // Redirect to dashboard
        $redirect = $_GET['redirect'] ?? 'index.php';
        header('Location: ' . $redirect);
        exit;
    } else {
        $error = 'Invalid username or password';
    }
}
?>
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Login - CariTranscoder</title>
    <link href="https://cdn.jsdelivr.net/npm/bootstrap@5.3.0/dist/css/bootstrap.min.css" rel="stylesheet">
    <style>
        * {
            margin: 0;
            padding: 0;
            box-sizing: border-box;
        }
        body {
            background: linear-gradient(135deg, #1a1a2e 0%, #16213e 100%);
            min-height: 100vh;
            display: flex;
            align-items: center;
            justify-content: center;
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Oxygen, Ubuntu, sans-serif;
        }
        .login-container {
            width: 100%;
            max-width: 420px;
            padding: 20px;
        }
        .login-card {
            background: rgba(255, 255, 255, 0.98);
            border-radius: 12px;
            box-shadow: 0 20px 40px rgba(0, 0, 0, 0.4);
            overflow: hidden;
        }
        .login-header {
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            color: white;
            padding: 35px 30px;
            text-align: center;
        }
        .login-header h1 {
            font-size: 1.9rem;
            font-weight: 600;
            margin-bottom: 8px;
        }
        .login-header p {
            opacity: 0.9;
            font-size: 0.95rem;
        }
        .login-body {
            padding: 35px 30px;
        }
        .form-group {
            margin-bottom: 20px;
        }
        .form-group label {
            display: block;
            font-weight: 500;
            color: #333;
            margin-bottom: 8px;
            font-size: 0.95rem;
        }
        .form-group input {
            width: 100%;
            padding: 14px 16px;
            border: 2px solid #e1e1e1;
            border-radius: 8px;
            font-size: 1rem;
            transition: border-color 0.2s, box-shadow 0.2s;
            outline: none;
        }
        .form-group input:focus {
            border-color: #667eea;
            box-shadow: 0 0 0 3px rgba(102, 126, 234, 0.2);
        }
        .form-group input::placeholder {
            color: #aaa;
        }
        .btn-login {
            width: 100%;
            padding: 14px;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            border: none;
            border-radius: 8px;
            color: white;
            font-size: 1.05rem;
            font-weight: 600;
            cursor: pointer;
            transition: transform 0.2s, box-shadow 0.2s;
            margin-top: 10px;
        }
        .btn-login:hover {
            transform: translateY(-2px);
            box-shadow: 0 5px 20px rgba(102, 126, 234, 0.4);
        }
        .btn-login:active {
            transform: translateY(0);
        }
        .alert-error {
            background: #fee2e2;
            border: 1px solid #fecaca;
            color: #dc2626;
            padding: 12px 16px;
            border-radius: 8px;
            margin-bottom: 20px;
            font-size: 0.9rem;
        }
        .login-footer {
            text-align: center;
            padding: 20px;
            color: rgba(255, 255, 255, 0.6);
            font-size: 0.85rem;
        }
    </style>
</head>
<body>
    <div class="login-container">
        <div class="login-card">
            <div class="login-header">
                <h1>CariTranscoder</h1>
                <p>Video Transcoding Management System</p>
            </div>
            <div class="login-body">
                <?php if ($error): ?>
                <div class="alert-error">
                    <?php echo htmlspecialchars($error); ?>
                </div>
                <?php endif; ?>

                <form method="POST" action="">
                    <div class="form-group">
                        <label for="username">Username</label>
                        <input type="text" id="username" name="username"
                               value="<?php echo htmlspecialchars($username); ?>"
                               placeholder="Enter your username" required autofocus>
                    </div>
                    <div class="form-group">
                        <label for="password">Password</label>
                        <input type="password" id="password" name="password"
                               placeholder="Enter your password" required>
                    </div>
                    <button type="submit" class="btn-login">
                        Sign In
                    </button>
                </form>
            </div>
        </div>
        <div class="login-footer">
            CariTranscoder v<?php echo CARITRANS_VERSION; ?> &copy; <?php echo date('Y'); ?> CariTech Solutions
        </div>
    </div>
</body>
</html>
