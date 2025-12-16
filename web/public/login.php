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
    <link href="css/bootstrap.min.css" rel="stylesheet">
    <link href="css/caritrans.css" rel="stylesheet">
    <style>
        body {
            background: linear-gradient(135deg, #1a1a2e 0%, #16213e 100%);
            min-height: 100vh;
            display: flex;
            align-items: center;
            justify-content: center;
        }
        .login-container {
            width: 100%;
            max-width: 400px;
            padding: 15px;
        }
        .login-card {
            background: rgba(255, 255, 255, 0.95);
            border-radius: 10px;
            box-shadow: 0 15px 35px rgba(0, 0, 0, 0.5);
        }
        .login-header {
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            color: white;
            padding: 30px;
            border-radius: 10px 10px 0 0;
            text-align: center;
        }
        .login-header h1 {
            font-size: 1.8rem;
            margin-bottom: 5px;
        }
        .login-header p {
            opacity: 0.9;
            margin: 0;
            font-size: 0.9rem;
        }
        .login-body {
            padding: 30px;
        }
        .form-control {
            border-radius: 5px;
            padding: 12px 15px;
            border: 1px solid #ddd;
        }
        .form-control:focus {
            border-color: #667eea;
            box-shadow: 0 0 0 0.2rem rgba(102, 126, 234, 0.25);
        }
        .btn-login {
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            border: none;
            padding: 12px;
            font-weight: 600;
            border-radius: 5px;
            width: 100%;
        }
        .btn-login:hover {
            opacity: 0.9;
            transform: translateY(-1px);
        }
        .alert-danger {
            border-radius: 5px;
            font-size: 0.9rem;
        }
        .login-footer {
            text-align: center;
            padding: 15px;
            color: rgba(255, 255, 255, 0.6);
            font-size: 0.8rem;
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
                <div class="alert alert-danger" role="alert">
                    <?php echo htmlspecialchars($error); ?>
                </div>
                <?php endif; ?>

                <form method="POST" action="">
                    <div class="mb-3">
                        <label for="username" class="form-label">Username</label>
                        <input type="text" class="form-control" id="username" name="username"
                               value="<?php echo htmlspecialchars($username); ?>"
                               placeholder="Enter username" required autofocus>
                    </div>
                    <div class="mb-4">
                        <label for="password" class="form-label">Password</label>
                        <input type="password" class="form-control" id="password" name="password"
                               placeholder="Enter password" required>
                    </div>
                    <button type="submit" class="btn btn-primary btn-login">
                        Sign In
                    </button>
                </form>
            </div>
        </div>
        <div class="login-footer">
            CariTranscoder v<?php echo CARITRANS_VERSION; ?> &copy; <?php echo date('Y'); ?> CariTech Solutions
        </div>
    </div>

    <script src="js/bootstrap.bundle.min.js"></script>
</body>
</html>
