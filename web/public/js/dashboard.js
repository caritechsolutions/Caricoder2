/**
 * CariTranscoder - Dashboard JavaScript
 * Copyright (c) 2024 CariTech Solutions
 */

// Dashboard specific functions

/**
 * Refresh dashboard stats
 */
async function refreshDashboardStats() {
    try {
        const stats = await apiRequest('stats.php');

        // Update stat cards
        if (document.getElementById('stat-inputs')) {
            document.getElementById('stat-inputs').textContent = stats.inputs_total || 0;
            document.getElementById('stat-inputs-running').textContent =
                (stats.inputs_running || 0) + ' running';
        }

        if (document.getElementById('stat-transcoders')) {
            document.getElementById('stat-transcoders').textContent = stats.transcoders_total || 0;
            document.getElementById('stat-transcoders-running').textContent =
                (stats.transcoders_running || 0) + ' running';
        }

        if (document.getElementById('stat-muxers')) {
            document.getElementById('stat-muxers').textContent = stats.muxers_total || 0;
            document.getElementById('stat-muxers-running').textContent =
                (stats.muxers_running || 0) + ' running';
        }

        if (document.getElementById('stat-outputs')) {
            document.getElementById('stat-outputs').textContent = stats.outputs_total || 0;
            document.getElementById('stat-outputs-running').textContent =
                (stats.outputs_running || 0) + ' running';
        }

        // Update system resources
        updateSystemStats(stats);

    } catch (error) {
        console.error('Failed to refresh dashboard stats:', error);
    }
}

/**
 * Refresh pipeline bitrates
 */
async function refreshPipelineBitrates() {
    try {
        const result = await apiRequest('pipelines.php?action=stats');

        if (result.pipelines) {
            result.pipelines.forEach(p => {
                const el = document.querySelector(`.bitrate[data-pipeline="${p.id}"]`);
                if (el) {
                    el.textContent = formatBitrate(p.bitrate);
                }

                // Update status badge
                const row = document.querySelector(`tr[data-pipeline-id="${p.id}"]`);
                if (row) {
                    const badge = row.querySelector('.status-badge');
                    if (badge) {
                        badge.className = `badge status-badge status-${p.status}`;
                        badge.textContent = capitalize(p.status);
                    }
                }
            });
        }
    } catch (error) {
        console.error('Failed to refresh bitrates:', error);
    }
}

// Dashboard initialization
document.addEventListener('DOMContentLoaded', () => {
    // Initial refresh
    refreshDashboardStats();

    // Periodic refresh
    setInterval(refreshDashboardStats, 5000);
    setInterval(refreshPipelineBitrates, 2000);
});
