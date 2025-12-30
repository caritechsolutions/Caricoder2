<?php
/**
 * CariTranscoder - Edit Transcoder
 * Copyright (c) 2024 CariTech Solutions
 */

define('CARITRANS', true);
require_once __DIR__ . '/../includes/config.php';
require_once __DIR__ . '/../includes/auth.php';
require_once __DIR__ . '/../includes/functions.php';

auth_require_login();

$id = $_GET['id'] ?? '';
$is_new = empty($id);

// Default values for new transcoder
$config = [
    'general' => [
        'name' => '',
        'enabled' => true
    ],
    'input' => [
        'address' => '',
        'port' => 5000
    ],
    'output' => [
        'address' => '',
        'port' => 5000,
        'api_port' => 9200,
        'video_pid' => 256,
        'audio_pid' => 257
    ],
    'video' => [
        'mode' => 'transcode',
        'codec' => 'h264',
        'encoder_type' => 'cpu',
        'bitrate' => 5000000,
        'preset' => 'superfast',
        'keyframe_interval' => 60,
        'profile' => 'main',
        'bframes' => 0,
        'ref' => 1,
        'qp_min' => 10,
        'qp_max' => 51,
        'vbv_bufsize' => 600,
        'threads' => 0,
        'sliced_threads' => false,
        'cabac' => true,
        'trellis' => false,
        'aud' => true,
        'intra_refresh' => false,
        'interlaced' => false,
        'psy_tune' => '',
        'x264_opts' => ''
    ],
    'scaling' => [
        'enabled' => false,
        'width' => 1920,
        'height' => 1080,
        'method' => 1,
        'add_borders' => false,
        'threads' => 0,
        'deinterlace' => false
    ],
    'audio' => [
        'mode' => 'transcode',
        'codec' => 'aac',
        'bitrate' => 128000,
        'channels' => 2,
        'samplerate' => 48000
    ]
];

if (!$is_new) {
    // Load existing config
    $id = preg_replace('/[^a-zA-Z0-9_-]/', '', $id);
    $config_file = CONFIG_PATH . '/transcoders/' . $id . '.conf';

    if (!file_exists($config_file)) {
        header('Location: transcoders.php?error=notfound');
        exit;
    }

    $loaded_config = parse_config($config_file);
    // Merge loaded config with defaults
    foreach ($loaded_config as $section => $values) {
        if (isset($config[$section])) {
            $config[$section] = array_merge($config[$section], $values);
        } else {
            $config[$section] = $values;
        }
    }
}

$page_title = $is_new ? 'New Transcoder' : 'Edit Transcoder: ' . ($config['general']['name'] ?? $id);
include __DIR__ . '/../templates/header.php';
?>

<div class="container-fluid py-4">
    <div class="d-flex justify-content-between align-items-center mb-4">
        <h2>
            <i class="bi bi-<?php echo $is_new ? 'plus-lg' : 'pencil'; ?> me-2"></i>
            <?php echo $is_new ? 'New Transcoder' : 'Edit Transcoder: ' . htmlspecialchars($config['general']['name'] ?? $id); ?>
        </h2>
        <a href="transcoders.php" class="btn btn-secondary">
            <i class="bi bi-arrow-left me-1"></i>Back to Transcoders
        </a>
    </div>

    <form id="transcoderForm">
        <?php if (!$is_new): ?>
        <input type="hidden" name="id" value="<?php echo htmlspecialchars($id); ?>">
        <?php endif; ?>

        <!-- Input Source Selection -->
        <div class="card mb-4">
            <div class="card-header">
                <h5 class="mb-0"><i class="bi bi-box-arrow-in-right me-2"></i>Input Source</h5>
            </div>
            <div class="card-body">
                <?php if ($is_new): ?>
                <div class="row">
                    <div class="col-md-6 mb-3">
                        <label class="form-label">Select Input <span class="text-danger">*</span></label>
                        <select class="form-select" name="input_source" id="inputSource" required onchange="onInputChange()">
                            <option value="">-- Select an Input --</option>
                        </select>
                        <div class="form-text">Select an existing input stream to transcode</div>
                    </div>
                    <div class="col-md-6 mb-3">
                        <label class="form-label">Transcoder Name</label>
                        <input type="text" class="form-control" name="name" id="transcoderName"
                               value="<?php echo htmlspecialchars($config['general']['name'] ?? ''); ?>" readonly>
                        <div class="form-text">Auto-generated based on input selection</div>
                        <input type="hidden" name="transcoder_id" id="transcoderId" value="">
                    </div>
                </div>
                <?php else: ?>
                <div class="row">
                    <div class="col-md-6 mb-3">
                        <label class="form-label">Transcoder Name</label>
                        <input type="text" class="form-control" name="name" id="transcoderName"
                               value="<?php echo htmlspecialchars($config['general']['name'] ?? ''); ?>" readonly>
                    </div>
                    <div class="col-md-6 mb-3">
                        <label class="form-label">Transcoder ID</label>
                        <input type="text" class="form-control" name="transcoder_id" id="transcoderId"
                               value="<?php echo htmlspecialchars($id); ?>" readonly>
                    </div>
                </div>
                <?php endif; ?>
                <div class="row">
                    <div class="col-md-6 mb-3">
                        <label class="form-label">Input Multicast Address</label>
                        <input type="text" class="form-control" name="input_address" id="inputAddress"
                               value="<?php echo htmlspecialchars($config['input']['address'] ?? ''); ?>"
                               placeholder="239.100.0.1" <?php echo $is_new ? 'readonly' : ''; ?>>
                    </div>
                    <div class="col-md-6 mb-3">
                        <label class="form-label">Input Port</label>
                        <input type="number" class="form-control" name="input_port" id="inputPort"
                               value="<?php echo htmlspecialchars($config['input']['port'] ?? '5000'); ?>"
                               min="1" max="65535" <?php echo $is_new ? 'readonly' : ''; ?>>
                    </div>
                </div>
            </div>
        </div>

        <!-- Output Configuration -->
        <div class="card mb-4">
            <div class="card-header">
                <h5 class="mb-0"><i class="bi bi-box-arrow-right me-2"></i>Output Destination</h5>
            </div>
            <div class="card-body">
                <div class="row">
                    <div class="col-md-4 mb-3">
                        <label class="form-label">Multicast Address <span class="text-danger">*</span></label>
                        <input type="text" class="form-control" name="output_address"
                               value="<?php echo htmlspecialchars($config['output']['address'] ?? ''); ?>"
                               placeholder="239.100.0.100" required>
                    </div>
                    <div class="col-md-4 mb-3">
                        <label class="form-label">Port <span class="text-danger">*</span></label>
                        <input type="number" class="form-control" name="output_port"
                               value="<?php echo htmlspecialchars($config['output']['port'] ?? '5000'); ?>"
                               min="1" max="65535" required>
                    </div>
                    <div class="col-md-4 mb-3">
                        <label class="form-label">API Port</label>
                        <input type="number" class="form-control" name="api_port"
                               value="<?php echo htmlspecialchars($config['output']['api_port'] ?? '9200'); ?>"
                               min="1024" max="65535">
                        <div class="form-text">For monitoring/stats</div>
                    </div>
                </div>
                <div class="row">
                    <div class="col-md-3 mb-3">
                        <label class="form-label">Video PID</label>
                        <input type="number" class="form-control" name="video_pid"
                               value="<?php echo htmlspecialchars($config['output']['video_pid'] ?? '256'); ?>"
                               min="32" max="8190">
                        <div class="form-text">Default: 256 (0x100)</div>
                    </div>
                    <div class="col-md-3 mb-3">
                        <label class="form-label">Audio PID</label>
                        <input type="number" class="form-control" name="audio_pid"
                               value="<?php echo htmlspecialchars($config['output']['audio_pid'] ?? '257'); ?>"
                               min="32" max="8190">
                        <div class="form-text">Default: 257 (0x101)</div>
                    </div>
                    <div class="col-md-6 mb-3">
                        <label class="form-label">&nbsp;</label>
                        <div class="alert alert-info py-2 mb-0 small">
                            <i class="bi bi-info-circle me-1"></i>
                            PIDs must be unique. Video and audio PIDs are used for bitrate monitoring.
                        </div>
                    </div>
                </div>
            </div>
        </div>

        <!-- Video Encoding -->
        <div class="card mb-4">
            <div class="card-header d-flex justify-content-between align-items-center">
                <h5 class="mb-0"><i class="bi bi-camera-video me-2"></i>Video Encoding (CPU)</h5>
                <span class="badge bg-info">Software Encoder</span>
            </div>
            <div class="card-body">
                <!-- Video Mode and Codec -->
                <div class="row">
                    <div class="col-md-3 mb-3">
                        <label class="form-label">Mode</label>
                        <select class="form-select" name="video_mode" id="videoMode" onchange="toggleVideoOptions()">
                            <option value="transcode" <?php echo ($config['video']['mode'] ?? '') === 'transcode' ? 'selected' : ''; ?>>Transcode</option>
                            <option value="passthrough" <?php echo ($config['video']['mode'] ?? '') === 'passthrough' ? 'selected' : ''; ?>>Passthrough</option>
                            <option value="drop" <?php echo ($config['video']['mode'] ?? '') === 'drop' ? 'selected' : ''; ?>>Drop</option>
                        </select>
                    </div>
                    <div class="col-md-3 mb-3 video-transcode-option">
                        <label class="form-label">Output Codec</label>
                        <select class="form-select" name="video_codec" id="videoCodec" onchange="toggleCodecOptions()">
                            <option value="h264" <?php echo ($config['video']['codec'] ?? '') === 'h264' ? 'selected' : ''; ?>>H.264 (AVC)</option>
                            <option value="h265" <?php echo ($config['video']['codec'] ?? '') === 'h265' ? 'selected' : ''; ?>>H.265 (HEVC)</option>
                            <option value="mpeg2" <?php echo ($config['video']['codec'] ?? '') === 'mpeg2' ? 'selected' : ''; ?>>MPEG-2</option>
                        </select>
                    </div>
                    <div class="col-md-3 mb-3 video-transcode-option">
                        <label class="form-label">Bitrate (bps)</label>
                        <input type="number" class="form-control" name="video_bitrate"
                               value="<?php echo htmlspecialchars($config['video']['bitrate'] ?? '5000000'); ?>"
                               min="100000" max="50000000" step="100000">
                        <div class="form-text">e.g., 5000000 = 5 Mbps</div>
                    </div>
                    <div class="col-md-3 mb-3 video-transcode-option">
                        <label class="form-label">Preset</label>
                        <select class="form-select" name="video_preset">
                            <?php
                            $presets = ['ultrafast', 'superfast', 'veryfast', 'faster', 'fast', 'medium', 'slow', 'slower', 'veryslow'];
                            foreach ($presets as $preset):
                            ?>
                            <option value="<?php echo $preset; ?>" <?php echo ($config['video']['preset'] ?? 'superfast') === $preset ? 'selected' : ''; ?>><?php echo ucfirst($preset); ?></option>
                            <?php endforeach; ?>
                        </select>
                    </div>
                </div>

                <!-- x264/x265 Common Options -->
                <div class="video-transcode-option">
                    <hr>
                    <h6 class="text-muted mb-3">Encoder Settings</h6>
                    <div class="row">
                        <div class="col-md-3 mb-3 x264-option">
                            <label class="form-label">Profile</label>
                            <select class="form-select" name="video_profile">
                                <option value="baseline" <?php echo ($config['video']['profile'] ?? '') === 'baseline' ? 'selected' : ''; ?>>Baseline</option>
                                <option value="main" <?php echo ($config['video']['profile'] ?? 'main') === 'main' ? 'selected' : ''; ?>>Main</option>
                                <option value="high" <?php echo ($config['video']['profile'] ?? '') === 'high' ? 'selected' : ''; ?>>High</option>
                            </select>
                        </div>
                        <div class="col-md-3 mb-3">
                            <label class="form-label">Keyframe Interval</label>
                            <input type="number" class="form-control" name="keyframe_interval"
                                   value="<?php echo htmlspecialchars($config['video']['keyframe_interval'] ?? '60'); ?>"
                                   min="1" max="300">
                            <div class="form-text">GOP size in frames</div>
                        </div>
                        <div class="col-md-3 mb-3">
                            <label class="form-label">Threads</label>
                            <input type="number" class="form-control" name="video_threads"
                                   value="<?php echo htmlspecialchars($config['video']['threads'] ?? '0'); ?>"
                                   min="0" max="64">
                            <div class="form-text">0 = auto</div>
                        </div>
                        <div class="col-md-3 mb-3 x264-option">
                            <label class="form-label">VBV Buffer (ms)</label>
                            <input type="number" class="form-control" name="vbv_bufsize"
                                   value="<?php echo htmlspecialchars($config['video']['vbv_bufsize'] ?? '600'); ?>"
                                   min="0" max="10000">
                        </div>
                    </div>

                    <!-- x264 Advanced Options -->
                    <div class="x264-option">
                        <div class="row">
                            <div class="col-md-3 mb-3">
                                <label class="form-label">B-Frames</label>
                                <input type="number" class="form-control" name="bframes"
                                       value="<?php echo htmlspecialchars($config['video']['bframes'] ?? '0'); ?>"
                                       min="0" max="16">
                            </div>
                            <div class="col-md-3 mb-3">
                                <label class="form-label">Reference Frames</label>
                                <input type="number" class="form-control" name="ref"
                                       value="<?php echo htmlspecialchars($config['video']['ref'] ?? '1'); ?>"
                                       min="1" max="12">
                            </div>
                            <div class="col-md-3 mb-3">
                                <label class="form-label">QP Min</label>
                                <input type="number" class="form-control" name="qp_min"
                                       value="<?php echo htmlspecialchars($config['video']['qp_min'] ?? '10'); ?>"
                                       min="0" max="51">
                            </div>
                            <div class="col-md-3 mb-3">
                                <label class="form-label">QP Max</label>
                                <input type="number" class="form-control" name="qp_max"
                                       value="<?php echo htmlspecialchars($config['video']['qp_max'] ?? '51'); ?>"
                                       min="0" max="51">
                            </div>
                        </div>
                        <div class="row">
                            <div class="col-md-3 mb-3">
                                <label class="form-label">Psy Tune</label>
                                <select class="form-select" name="psy_tune">
                                    <option value="" <?php echo empty($config['video']['psy_tune']) ? 'selected' : ''; ?>>None</option>
                                    <option value="film" <?php echo ($config['video']['psy_tune'] ?? '') === 'film' ? 'selected' : ''; ?>>Film</option>
                                    <option value="animation" <?php echo ($config['video']['psy_tune'] ?? '') === 'animation' ? 'selected' : ''; ?>>Animation</option>
                                    <option value="grain" <?php echo ($config['video']['psy_tune'] ?? '') === 'grain' ? 'selected' : ''; ?>>Grain</option>
                                    <option value="psnr" <?php echo ($config['video']['psy_tune'] ?? '') === 'psnr' ? 'selected' : ''; ?>>PSNR</option>
                                    <option value="ssim" <?php echo ($config['video']['psy_tune'] ?? '') === 'ssim' ? 'selected' : ''; ?>>SSIM</option>
                                </select>
                            </div>
                            <div class="col-md-9 mb-3">
                                <label class="form-label">Custom x264 Options</label>
                                <input type="text" class="form-control" name="x264_opts"
                                       value="<?php echo htmlspecialchars($config['video']['x264_opts'] ?? ''); ?>"
                                       placeholder="key=value:key=value">
                            </div>
                        </div>
                        <div class="row">
                            <div class="col-md-12">
                                <div class="d-flex flex-wrap gap-3">
                                    <div class="form-check">
                                        <input class="form-check-input" type="checkbox" name="sliced_threads" id="slicedThreads"
                                               <?php echo config_bool($config['video']['sliced_threads'] ?? false) ? 'checked' : ''; ?>>
                                        <label class="form-check-label" for="slicedThreads">Sliced Threads</label>
                                    </div>
                                    <div class="form-check">
                                        <input class="form-check-input" type="checkbox" name="cabac" id="cabac"
                                               <?php echo config_bool($config['video']['cabac'] ?? true) ? 'checked' : ''; ?>>
                                        <label class="form-check-label" for="cabac">CABAC</label>
                                    </div>
                                    <div class="form-check">
                                        <input class="form-check-input" type="checkbox" name="trellis" id="trellis"
                                               <?php echo config_bool($config['video']['trellis'] ?? false) ? 'checked' : ''; ?>>
                                        <label class="form-check-label" for="trellis">Trellis</label>
                                    </div>
                                    <div class="form-check">
                                        <input class="form-check-input" type="checkbox" name="aud" id="aud"
                                               <?php echo config_bool($config['video']['aud'] ?? true) ? 'checked' : ''; ?>>
                                        <label class="form-check-label" for="aud">AUD</label>
                                    </div>
                                    <div class="form-check">
                                        <input class="form-check-input" type="checkbox" name="intra_refresh" id="intraRefresh"
                                               <?php echo config_bool($config['video']['intra_refresh'] ?? false) ? 'checked' : ''; ?>>
                                        <label class="form-check-label" for="intraRefresh">Intra Refresh</label>
                                    </div>
                                    <div class="form-check">
                                        <input class="form-check-input" type="checkbox" name="interlaced" id="interlaced"
                                               <?php echo config_bool($config['video']['interlaced'] ?? false) ? 'checked' : ''; ?>>
                                        <label class="form-check-label" for="interlaced">Interlaced</label>
                                    </div>
                                </div>
                            </div>
                        </div>
                    </div>
                </div>
            </div>
        </div>

        <!-- Video Scaling -->
        <div class="card mb-4">
            <div class="card-header">
                <h5 class="mb-0">
                    <div class="form-check form-switch d-inline-block me-2">
                        <input class="form-check-input" type="checkbox" name="scaling_enabled" id="scalingEnabled"
                               onchange="toggleScalingOptions()"
                               <?php echo config_bool($config['scaling']['enabled'] ?? false) ? 'checked' : ''; ?>>
                    </div>
                    <i class="bi bi-arrows-angle-expand me-2"></i>Video Scaling
                </h5>
            </div>
            <div class="card-body" id="scalingOptions">
                <div class="row">
                    <div class="col-md-2 mb-3">
                        <label class="form-label">Width</label>
                        <input type="number" class="form-control" name="scale_width"
                               value="<?php echo htmlspecialchars($config['scaling']['width'] ?? '1920'); ?>"
                               min="128" max="7680">
                    </div>
                    <div class="col-md-2 mb-3">
                        <label class="form-label">Height</label>
                        <input type="number" class="form-control" name="scale_height"
                               value="<?php echo htmlspecialchars($config['scaling']['height'] ?? '1080'); ?>"
                               min="96" max="4320">
                    </div>
                    <div class="col-md-3 mb-3">
                        <label class="form-label">Method</label>
                        <select class="form-select" name="scale_method">
                            <option value="0" <?php echo ($config['scaling']['method'] ?? 1) == 0 ? 'selected' : ''; ?>>Nearest (fastest)</option>
                            <option value="1" <?php echo ($config['scaling']['method'] ?? 1) == 1 ? 'selected' : ''; ?>>Bilinear (default)</option>
                            <option value="2" <?php echo ($config['scaling']['method'] ?? 1) == 2 ? 'selected' : ''; ?>>4-tap</option>
                            <option value="3" <?php echo ($config['scaling']['method'] ?? 1) == 3 ? 'selected' : ''; ?>>Lanczos (quality)</option>
                            <option value="4" <?php echo ($config['scaling']['method'] ?? 1) == 4 ? 'selected' : ''; ?>>Bilinear2</option>
                            <option value="5" <?php echo ($config['scaling']['method'] ?? 1) == 5 ? 'selected' : ''; ?>>Sinc</option>
                            <option value="6" <?php echo ($config['scaling']['method'] ?? 1) == 6 ? 'selected' : ''; ?>>Hermite</option>
                            <option value="7" <?php echo ($config['scaling']['method'] ?? 1) == 7 ? 'selected' : ''; ?>>Spline</option>
                            <option value="8" <?php echo ($config['scaling']['method'] ?? 1) == 8 ? 'selected' : ''; ?>>Catmull-Rom</option>
                            <option value="9" <?php echo ($config['scaling']['method'] ?? 1) == 9 ? 'selected' : ''; ?>>Mitchell</option>
                        </select>
                    </div>
                    <div class="col-md-2 mb-3">
                        <label class="form-label">Threads</label>
                        <input type="number" class="form-control" name="scale_threads"
                               value="<?php echo htmlspecialchars($config['scaling']['threads'] ?? '0'); ?>"
                               min="0" max="64">
                    </div>
                    <div class="col-md-3 mb-3">
                        <label class="form-label">Options</label>
                        <div class="d-flex flex-wrap gap-3 mt-2">
                            <div class="form-check">
                                <input class="form-check-input" type="checkbox" name="add_borders" id="addBorders"
                                       <?php echo config_bool($config['scaling']['add_borders'] ?? false) ? 'checked' : ''; ?>>
                                <label class="form-check-label" for="addBorders">Add Borders</label>
                            </div>
                            <div class="form-check">
                                <input class="form-check-input" type="checkbox" name="deinterlace" id="deinterlace"
                                       <?php echo config_bool($config['scaling']['deinterlace'] ?? false) ? 'checked' : ''; ?>>
                                <label class="form-check-label" for="deinterlace">Deinterlace</label>
                            </div>
                        </div>
                    </div>
                </div>
                <div class="row">
                    <div class="col-12">
                        <div class="btn-group" role="group">
                            <button type="button" class="btn btn-outline-secondary btn-sm" onclick="setResolution(1920,1080)">1080p</button>
                            <button type="button" class="btn btn-outline-secondary btn-sm" onclick="setResolution(1280,720)">720p</button>
                            <button type="button" class="btn btn-outline-secondary btn-sm" onclick="setResolution(854,480)">480p</button>
                            <button type="button" class="btn btn-outline-secondary btn-sm" onclick="setResolution(640,360)">360p</button>
                        </div>
                    </div>
                </div>
            </div>
        </div>

        <!-- Audio Encoding -->
        <div class="card mb-4">
            <div class="card-header">
                <h5 class="mb-0"><i class="bi bi-music-note-beamed me-2"></i>Audio Encoding</h5>
            </div>
            <div class="card-body">
                <div class="row">
                    <div class="col-md-3 mb-3">
                        <label class="form-label">Mode</label>
                        <select class="form-select" name="audio_mode" id="audioMode" onchange="toggleAudioOptions()">
                            <option value="transcode" <?php echo ($config['audio']['mode'] ?? '') === 'transcode' ? 'selected' : ''; ?>>Transcode</option>
                            <option value="passthrough" <?php echo ($config['audio']['mode'] ?? '') === 'passthrough' ? 'selected' : ''; ?>>Passthrough</option>
                            <option value="drop" <?php echo ($config['audio']['mode'] ?? '') === 'drop' ? 'selected' : ''; ?>>Drop</option>
                        </select>
                    </div>
                    <div class="col-md-3 mb-3 audio-transcode-option">
                        <label class="form-label">Codec</label>
                        <select class="form-select" name="audio_codec" id="audioCodec" onchange="toggleAudioCodecOptions()">
                            <option value="aac" <?php echo ($config['audio']['codec'] ?? '') === 'aac' ? 'selected' : ''; ?>>AAC</option>
                            <option value="ac3" <?php echo ($config['audio']['codec'] ?? '') === 'ac3' ? 'selected' : ''; ?>>AC3</option>
                            <option value="mp2" <?php echo ($config['audio']['codec'] ?? '') === 'mp2' ? 'selected' : ''; ?>>MP2</option>
                        </select>
                    </div>
                    <div class="col-md-3 mb-3 audio-transcode-option">
                        <label class="form-label">Bitrate (bps)</label>
                        <input type="number" class="form-control" name="audio_bitrate"
                               value="<?php echo htmlspecialchars($config['audio']['bitrate'] ?? '128000'); ?>"
                               min="32000" max="512000" step="8000">
                    </div>
                    <div class="col-md-3 mb-3 audio-transcode-option">
                        <label class="form-label">Channels</label>
                        <select class="form-select" name="audio_channels">
                            <option value="1" <?php echo ($config['audio']['channels'] ?? 2) == 1 ? 'selected' : ''; ?>>Mono</option>
                            <option value="2" <?php echo ($config['audio']['channels'] ?? 2) == 2 ? 'selected' : ''; ?>>Stereo</option>
                            <option value="6" <?php echo ($config['audio']['channels'] ?? 2) == 6 ? 'selected' : ''; ?>>5.1 Surround</option>
                        </select>
                    </div>
                </div>
                <div class="row audio-transcode-option">
                    <div class="col-md-3 mb-3">
                        <label class="form-label">Sample Rate</label>
                        <select class="form-select" name="audio_samplerate">
                            <option value="48000" <?php echo ($config['audio']['samplerate'] ?? 48000) == 48000 ? 'selected' : ''; ?>>48000 Hz</option>
                            <option value="44100" <?php echo ($config['audio']['samplerate'] ?? 48000) == 44100 ? 'selected' : ''; ?>>44100 Hz</option>
                            <option value="32000" <?php echo ($config['audio']['samplerate'] ?? 48000) == 32000 ? 'selected' : ''; ?>>32000 Hz</option>
                        </select>
                    </div>
                </div>

            </div>
        </div>

        <!-- Action Buttons -->
        <div class="d-flex gap-2">
            <button type="submit" class="btn btn-primary">
                <i class="bi bi-check-lg me-1"></i><?php echo $is_new ? 'Create Transcoder' : 'Save Changes'; ?>
            </button>
            <a href="transcoders.php" class="btn btn-secondary">Cancel</a>
            <?php if (!$is_new): ?>
            <button type="button" class="btn btn-danger ms-auto" onclick="deleteTranscoder()">
                <i class="bi bi-trash me-1"></i>Delete Transcoder
            </button>
            <?php endif; ?>
        </div>
    </form>
</div>

<script>
// Input data storage
let inputsData = [];

// Load inputs on page load
async function loadInputs() {
    const isNew = <?php echo $is_new ? 'true' : 'false'; ?>;
    if (!isNew) return;

    try {
        const response = await fetch('api/transcoders.php?action=inputs');
        const result = await response.json();
        if (result.success && result.inputs) {
            inputsData = result.inputs;
            const select = document.getElementById('inputSource');
            result.inputs.forEach(input => {
                const option = document.createElement('option');
                option.value = input.id;
                option.textContent = input.name;
                option.dataset.address = input.output_address;
                option.dataset.port = input.output_port;
                select.appendChild(option);
            });
        }
    } catch (err) {
        console.error('Failed to load inputs:', err);
    }
}

// Handle input selection change
async function onInputChange() {
    const select = document.getElementById('inputSource');
    const selectedOption = select.options[select.selectedIndex];

    if (!select.value) {
        document.getElementById('transcoderName').value = '';
        document.getElementById('transcoderId').value = '';
        document.getElementById('inputAddress').value = '';
        document.getElementById('inputPort').value = '';
        return;
    }

    // Set input address/port from selected input's output
    document.getElementById('inputAddress').value = selectedOption.dataset.address || '';
    document.getElementById('inputPort').value = selectedOption.dataset.port || '';

    // Get next available transcoder ID for this input
    try {
        const response = await fetch(`api/transcoders.php?action=next_id&input_id=${select.value}`);
        const result = await response.json();
        if (result.success) {
            document.getElementById('transcoderName').value = result.next_name;
            document.getElementById('transcoderId').value = result.next_id;
        }
    } catch (err) {
        console.error('Failed to get next ID:', err);
    }
}

// Initialize on page load
document.addEventListener('DOMContentLoaded', function() {
    loadInputs();
    toggleVideoOptions();
    toggleAudioOptions();
    toggleScalingOptions();
});

function toggleVideoOptions() {
    const mode = document.getElementById('videoMode').value;
    const options = document.querySelectorAll('.video-transcode-option');
    options.forEach(el => el.style.display = mode === 'transcode' ? '' : 'none');
    if (mode === 'transcode') toggleCodecOptions();
}

function toggleCodecOptions() {
    const codec = document.getElementById('videoCodec').value;
    const x264Options = document.querySelectorAll('.x264-option');
    x264Options.forEach(el => el.style.display = codec === 'h264' ? '' : 'none');
}

function toggleScalingOptions() {
    const enabled = document.getElementById('scalingEnabled').checked;
    document.getElementById('scalingOptions').style.opacity = enabled ? '1' : '0.5';
    const inputs = document.querySelectorAll('#scalingOptions input, #scalingOptions select');
    inputs.forEach(el => el.disabled = !enabled);
}

function toggleAudioOptions() {
    const mode = document.getElementById('audioMode').value;
    const options = document.querySelectorAll('.audio-transcode-option');
    options.forEach(el => el.style.display = mode === 'transcode' ? '' : 'none');
    if (mode === 'transcode') toggleAudioCodecOptions();
}

function toggleAudioCodecOptions() {
    const codec = document.getElementById('audioCodec').value;
    const aacOptions = document.querySelectorAll('.aac-option');
    aacOptions.forEach(el => el.style.display = codec === 'aac' ? '' : 'none');
}

function setResolution(w, h) {
    document.querySelector('input[name="scale_width"]').value = w;
    document.querySelector('input[name="scale_height"]').value = h;
}

// Form submission
document.getElementById('transcoderForm').addEventListener('submit', async function(e) {
    e.preventDefault();

    const formData = new FormData(this);
    const data = Object.fromEntries(formData);

    // Convert checkboxes to boolean
    const checkboxes = ['sliced_threads', 'cabac', 'trellis', 'aud', 'intra_refresh', 'interlaced',
                        'scaling_enabled', 'add_borders', 'deinterlace',
                        'aac_is', 'aac_ms', 'aac_pns', 'aac_tns', 'aac_ltp', 'aac_pred'];
    checkboxes.forEach(name => {
        data[name] = formData.has(name);
    });

    const isNew = <?php echo $is_new ? 'true' : 'false'; ?>;
    const action = isNew ? 'create' : 'update';

    try {
        const response = await fetch(`api/transcoders.php?action=${action}`, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(data)
        });

        const result = await response.json();
        if (result.success) {
            window.location.href = 'transcoders.php?success=' + action;
        } else {
            alert('Error: ' + (result.error || 'Unknown error'));
        }
    } catch (err) {
        alert('Error: ' + err.message);
    }
});

function deleteTranscoder() {
    if (confirm('Are you sure you want to delete this transcoder?')) {
        const id = '<?php echo htmlspecialchars($id); ?>';
        fetch(`api/transcoders.php?action=delete&id=${id}`, { method: 'POST' })
            .then(r => r.json())
            .then(data => {
                if (data.success) {
                    window.location.href = 'transcoders.php?success=deleted';
                } else {
                    alert('Error: ' + (data.error || 'Unknown error'));
                }
            });
    }
}
</script>

<?php include __DIR__ . '/../templates/footer.php'; ?>
