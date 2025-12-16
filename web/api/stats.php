<?php
/**
 * CariTranscoder - Stats API
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

// Get system stats
$stats = get_system_stats();

// Get service counts
$inputs = get_service_list('inputs');
$transcoders = get_service_list('transcoders');
$muxers = get_service_list('muxers');
$outputs = get_service_list('outputs');

$response = [
    // System stats
    'cpu_percent' => $stats['cpu_percent'],
    'mem_percent' => $stats['mem_percent'],
    'mem_used' => $stats['mem_used'],
    'mem_total' => $stats['mem_total'],
    'load_avg' => $stats['load_avg'],
    'net_rx' => $stats['net_rx'],
    'net_tx' => $stats['net_tx'],

    // Service counts
    'inputs_total' => count($inputs),
    'inputs_running' => count(array_filter($inputs, fn($i) => $i['status'] === 'running')),

    'transcoders_total' => count($transcoders),
    'transcoders_running' => count(array_filter($transcoders, fn($t) => $t['status'] === 'running')),

    'muxers_total' => count($muxers),
    'muxers_running' => count(array_filter($muxers, fn($m) => $m['status'] === 'running')),

    'outputs_total' => count($outputs),
    'outputs_running' => count(array_filter($outputs, fn($o) => $o['status'] === 'running')),

    // Cluster
    'cluster_enabled' => $stats['cluster_enabled'],

    // Timestamp
    'timestamp' => time()
];

json_response($response);
