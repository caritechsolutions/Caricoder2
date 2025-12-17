<?php
/**
 * CariTranscoder - Edit Input
 * Copyright (c) 2024 CariTech Solutions
 */

define('CARITRANS', true);
require_once __DIR__ . '/../includes/config.php';
require_once __DIR__ . '/../includes/auth.php';
require_once __DIR__ . '/../includes/functions.php';

auth_require_login();

$id = $_GET['id'] ?? '';
if (empty($id)) {
    header('Location: inputs.php');
    exit;
}

// Sanitize ID
$id = preg_replace('/[^a-zA-Z0-9_-]/', '', $id);
$config_file = CONFIG_DIR . '/inputs/' . $id . '.conf';

if (!file_exists($config_file)) {
    header('Location: inputs.php?error=notfound');
    exit;
}

$config = parse_config($config_file);
$input_name = $config['general']['name'] ?? $id;
$input_type = $config['general']['type'] ?? 'udp';
$input_buffer = $config['general']['buffer'] ?? '';

// Parse sources
$sources = [];
if (isset($config['sources'])) {
    foreach ($config['sources'] as $key => $value) {
        if (strpos($key, 'source_') === 0) {
            $parts = explode('|', $value);
            $source = [
                'type' => $parts[0] ?? 'udp',
                'url' => $parts[1] ?? '',
                'weight' => $parts[2] ?? 100
            ];
            // Parse extra settings
            if (isset($parts[3])) {
                $extras = explode(',', $parts[3]);
                foreach ($extras as $extra) {
                    $kv = explode('=', $extra, 2);
                    if (count($kv) == 2) {
                        $source[$kv[0]] = $kv[1];
                    }
                }
            }
            $sources[] = $source;
        }
    }
}

// Parse PIDs
$video_pid = $config['pids']['video'] ?? '';
$audio_pids = $config['pids']['audio'] ?? '';
$program_pid = $config['pids']['program'] ?? '';

$page_title = 'Edit Input: ' . $input_name;
include __DIR__ . '/../templates/header.php';
?>

<div class="container-fluid py-4">
    <div class="d-flex justify-content-between align-items-center mb-4">
        <h2><i class="bi bi-pencil me-2"></i>Edit Input: <?php echo htmlspecialchars($input_name); ?></h2>
        <a href="inputs.php" class="btn btn-secondary">
            <i class="bi bi-arrow-left me-1"></i>Back to Inputs
        </a>
    </div>

    <form id="editInputForm">
        <input type="hidden" name="id" value="<?php echo htmlspecialchars($id); ?>">

        <div class="card mb-4">
            <div class="card-header">
                <h5 class="mb-0">Basic Information</h5>
            </div>
            <div class="card-body">
                <div class="row">
                    <div class="col-md-6 mb-3">
                        <label class="form-label">Input Name</label>
                        <input type="text" class="form-control" name="name" value="<?php echo htmlspecialchars($input_name); ?>" required>
                    </div>
                    <div class="col-md-6 mb-3">
                        <label class="form-label">Output Buffer</label>
                        <input type="text" class="form-control" name="buffer" value="<?php echo htmlspecialchars($input_buffer); ?>">
                    </div>
                </div>
            </div>
        </div>

        <div class="card mb-4">
            <div class="card-header d-flex justify-content-between align-items-center">
                <h5 class="mb-0">Sources</h5>
                <button type="button" class="btn btn-sm btn-outline-primary" onclick="addSource()">
                    <i class="bi bi-plus-lg"></i> Add Failover Source
                </button>
            </div>
            <div class="card-body">
                <div id="sourcesContainer">
                    <?php if (empty($sources)): ?>
                    <!-- Will be populated by JS -->
                    <?php else: ?>
                    <?php foreach ($sources as $idx => $source): ?>
                    <div class="source-card <?php echo $idx === 0 ? 'primary' : ''; ?>" id="source-<?php echo $idx + 1; ?>" data-source-id="<?php echo $idx + 1; ?>">
                        <div class="d-flex justify-content-between align-items-start mb-3">
                            <span class="badge <?php echo $idx === 0 ? 'bg-primary' : 'bg-secondary'; ?>">
                                <?php echo $idx === 0 ? 'Primary Source' : 'Failover Source ' . $idx; ?>
                            </span>
                            <?php if ($idx > 0): ?>
                            <button type="button" class="btn btn-sm btn-outline-danger" onclick="removeSource(<?php echo $idx + 1; ?>)">
                                <i class="bi bi-trash"></i>
                            </button>
                            <?php endif; ?>
                        </div>
                        <div class="row">
                            <div class="col-md-3 mb-2">
                                <label class="form-label">Type</label>
                                <select class="form-select source-type" name="sources[<?php echo $idx; ?>][type]" onchange="updateSourceFields(<?php echo $idx + 1; ?>)">
                                    <option value="udp" <?php echo ($source['type'] ?? 'udp') === 'udp' ? 'selected' : ''; ?>>UDP Multicast</option>
                                    <option value="srt" <?php echo ($source['type'] ?? '') === 'srt' ? 'selected' : ''; ?>>SRT</option>
                                    <option value="rtmp" <?php echo ($source['type'] ?? '') === 'rtmp' ? 'selected' : ''; ?>>RTMP</option>
                                    <option value="hls" <?php echo ($source['type'] ?? '') === 'hls' ? 'selected' : ''; ?>>HLS</option>
                                    <option value="file" <?php echo ($source['type'] ?? '') === 'file' ? 'selected' : ''; ?>>File</option>
                                </select>
                            </div>
                            <div class="col-md-6 mb-2">
                                <label class="form-label source-url-label">Source URL</label>
                                <input type="text" class="form-control source-url" name="sources[<?php echo $idx; ?>][url]"
                                       value="<?php echo htmlspecialchars($source['url'] ?? ''); ?>" <?php echo $idx === 0 ? 'required' : ''; ?>>
                            </div>
                            <div class="col-md-3 mb-2">
                                <label class="form-label">Priority</label>
                                <input type="number" class="form-control source-weight" name="sources[<?php echo $idx; ?>][weight]"
                                       value="<?php echo htmlspecialchars($source['weight'] ?? 100); ?>" min="1" max="100">
                            </div>
                        </div>
                    </div>
                    <?php endforeach; ?>
                    <?php endif; ?>
                </div>
            </div>
        </div>

        <div class="card mb-4">
            <div class="card-header d-flex justify-content-between align-items-center">
                <h5 class="mb-0">PID Selection</h5>
                <button type="button" class="btn btn-info btn-sm" onclick="scanSource()">
                    <i class="bi bi-search me-1"></i>Scan Source
                </button>
            </div>
            <div class="card-body">
                <div class="row">
                    <div class="col-md-4 mb-3">
                        <label class="form-label">Program PID</label>
                        <input type="text" class="form-control" name="program_pid" value="<?php echo htmlspecialchars($program_pid); ?>">
                    </div>
                    <div class="col-md-4 mb-3">
                        <label class="form-label">Video PID</label>
                        <input type="text" class="form-control" name="video_pid" value="<?php echo htmlspecialchars($video_pid); ?>">
                    </div>
                    <div class="col-md-4 mb-3">
                        <label class="form-label">Audio PIDs</label>
                        <input type="text" class="form-control" name="audio_pids" value="<?php echo htmlspecialchars($audio_pids); ?>" placeholder="Comma-separated: 257,258">
                    </div>
                </div>
            </div>
        </div>

        <div class="d-flex gap-2">
            <button type="submit" class="btn btn-primary">
                <i class="bi bi-check-lg me-1"></i>Save Changes
            </button>
            <a href="inputs.php" class="btn btn-secondary">Cancel</a>
            <button type="button" class="btn btn-danger ms-auto" onclick="deleteInput()">
                <i class="bi bi-trash me-1"></i>Delete Input
            </button>
        </div>
    </form>
</div>

<style>
.source-card {
    background: #f8f9fa;
    border: 1px solid #dee2e6;
    border-radius: 0.375rem;
    padding: 1rem;
    margin-bottom: 1rem;
}
.source-card.primary {
    border-color: #0d6efd;
    background: #f0f7ff;
}
</style>

<script>
let sourcesCount = <?php echo max(1, count($sources)); ?>;
const inputId = '<?php echo htmlspecialchars($id); ?>';

document.addEventListener('DOMContentLoaded', function() {
    // Add first source if none exist
    if (sourcesCount === 0) {
        addSource();
    }

    document.getElementById('editInputForm').addEventListener('submit', handleSubmit);
});

function addSource() {
    sourcesCount++;
    const isPrimary = document.querySelectorAll('.source-card').length === 0;
    const container = document.getElementById('sourcesContainer');
    const idx = sourcesCount - 1;

    const sourceHtml = `
        <div class="source-card ${isPrimary ? 'primary' : ''}" id="source-${sourcesCount}" data-source-id="${sourcesCount}">
            <div class="d-flex justify-content-between align-items-start mb-3">
                <span class="badge ${isPrimary ? 'bg-primary' : 'bg-secondary'}">
                    ${isPrimary ? 'Primary Source' : 'Failover Source'}
                </span>
                ${!isPrimary ? `<button type="button" class="btn btn-sm btn-outline-danger" onclick="removeSource(${sourcesCount})">
                    <i class="bi bi-trash"></i>
                </button>` : ''}
            </div>
            <div class="row">
                <div class="col-md-3 mb-2">
                    <label class="form-label">Type</label>
                    <select class="form-select source-type" name="sources[${idx}][type]">
                        <option value="udp">UDP Multicast</option>
                        <option value="srt">SRT</option>
                        <option value="rtmp">RTMP</option>
                        <option value="hls">HLS</option>
                        <option value="file">File</option>
                    </select>
                </div>
                <div class="col-md-6 mb-2">
                    <label class="form-label source-url-label">Source URL</label>
                    <input type="text" class="form-control source-url" name="sources[${idx}][url]" ${isPrimary ? 'required' : ''}>
                </div>
                <div class="col-md-3 mb-2">
                    <label class="form-label">Priority</label>
                    <input type="number" class="form-control source-weight" name="sources[${idx}][weight]" value="${isPrimary ? 100 : 50}" min="1" max="100">
                </div>
            </div>
        </div>
    `;

    container.insertAdjacentHTML('beforeend', sourceHtml);
}

function removeSource(id) {
    const el = document.getElementById(`source-${id}`);
    if (el) el.remove();
}

async function scanSource() {
    const firstSource = document.querySelector('.source-card');
    const sourceInput = firstSource ? firstSource.querySelector('.source-url') : null;
    const sourceType = firstSource ? firstSource.querySelector('.source-type') : null;

    const source = sourceInput ? sourceInput.value : '';
    const type = sourceType ? sourceType.value : 'udp';

    if (!source) {
        alert('Please enter a source URL first');
        return;
    }

    try {
        const formData = new FormData();
        formData.append('source', source);
        formData.append('type', type);

        const response = await fetch('api/inputs.php?action=scan', {
            method: 'POST',
            body: formData
        });
        const data = await response.json();

        if (data.success) {
            // Populate PID fields
            if (data.video_pids && data.video_pids.length > 0) {
                document.querySelector('[name="video_pid"]').value = data.video_pids[0].pid;
            }
            if (data.audio_pids && data.audio_pids.length > 0) {
                document.querySelector('[name="audio_pids"]').value = data.audio_pids.map(a => a.pid).join(',');
            }
            if (data.programs && data.programs.length > 0) {
                document.querySelector('[name="program_pid"]').value = data.programs[0].id;
            }
            alert('Scan complete! PID fields updated.');
        } else {
            alert('Scan failed: ' + (data.error || 'Unknown error'));
        }
    } catch (e) {
        console.error('Scan error:', e);
        alert('Scan error');
    }
}

async function handleSubmit(e) {
    e.preventDefault();

    const form = e.target;
    const formData = new FormData(form);

    const data = {
        id: inputId,
        name: formData.get('name'),
        buffer: formData.get('buffer'),
        sources: [],
        video_pid: formData.get('video_pid'),
        audio_pids: formData.get('audio_pids'),
        program_pid: formData.get('program_pid')
    };

    // Collect sources
    document.querySelectorAll('.source-card').forEach((card, idx) => {
        const urlInput = card.querySelector('.source-url');
        const typeSelect = card.querySelector('.source-type');
        const weightInput = card.querySelector('.source-weight');

        if (urlInput && urlInput.value.trim()) {
            data.sources.push({
                url: urlInput.value.trim(),
                type: typeSelect ? typeSelect.value : 'udp',
                weight: parseInt(weightInput?.value || 10)
            });
        }
    });

    try {
        const response = await fetch(`api/inputs.php?action=update&id=${inputId}`, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(data)
        });

        const result = await response.json();

        if (result.success) {
            alert('Input updated successfully!');
            window.location.href = 'inputs.php';
        } else {
            alert('Error: ' + (result.error || 'Failed to update input'));
        }
    } catch (e) {
        console.error('Submit error:', e);
        alert('Error updating input');
    }
}

function deleteInput() {
    if (confirm('Are you sure you want to delete this input?')) {
        fetch(`api/inputs.php?action=delete&id=${inputId}`, { method: 'POST' })
            .then(r => r.json())
            .then(data => {
                if (data.success) {
                    window.location.href = 'inputs.php';
                } else {
                    alert(data.error || 'Failed to delete');
                }
            });
    }
}
</script>

<?php include __DIR__ . '/../templates/footer.php'; ?>
