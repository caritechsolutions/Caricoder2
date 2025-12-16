/*
 * CariTranscoder - MPEG-TS Packet Implementation
 * Copyright (c) 2024 CariTech Solutions
 */

#include <string.h>
#include <time.h>
#include "ts_packet.h"

int ts_parse_header(const ts_packet_raw_t *packet, ts_header_t *header) {
    if (!packet || !header) return -1;

    if (packet->data[0] != TS_SYNC_BYTE) {
        return -1;
    }

    header->sync_byte = packet->data[0];
    header->transport_error = (packet->data[1] & 0x80) != 0;
    header->payload_unit_start = (packet->data[1] & 0x40) != 0;
    header->transport_priority = (packet->data[1] & 0x20) != 0;
    header->pid = ((packet->data[1] & 0x1F) << 8) | packet->data[2];
    header->scrambling_control = (packet->data[3] >> 6) & 0x03;
    header->adaptation_field_control = (packet->data[3] >> 4) & 0x03;
    header->continuity_counter = packet->data[3] & 0x0F;

    return 0;
}

int ts_parse_adaptation_field(const ts_packet_raw_t *packet,
                               ts_adaptation_field_t *af) {
    if (!packet || !af) return -1;

    if (!ts_packet_has_adaptation(packet)) {
        return -1;
    }

    memset(af, 0, sizeof(ts_adaptation_field_t));

    af->length = packet->data[4];
    if (af->length == 0) {
        return 0;
    }

    uint8_t flags = packet->data[5];
    af->discontinuity = (flags & TS_AF_DISCONTINUITY) != 0;
    af->random_access = (flags & TS_AF_RANDOM_ACCESS) != 0;
    af->es_priority = (flags & TS_AF_ES_PRIORITY) != 0;
    af->pcr_flag = (flags & TS_AF_PCR) != 0;
    af->opcr_flag = (flags & TS_AF_OPCR) != 0;
    af->splicing_flag = (flags & TS_AF_SPLICING) != 0;
    af->private_data_flag = (flags & TS_AF_PRIVATE) != 0;
    af->extension_flag = (flags & TS_AF_EXTENSION) != 0;

    int offset = 6;

    /* Parse PCR */
    if (af->pcr_flag && (offset + 6) <= (4 + af->length)) {
        uint64_t pcr_base = ((uint64_t)packet->data[offset] << 25) |
                           ((uint64_t)packet->data[offset + 1] << 17) |
                           ((uint64_t)packet->data[offset + 2] << 9) |
                           ((uint64_t)packet->data[offset + 3] << 1) |
                           ((uint64_t)packet->data[offset + 4] >> 7);

        uint16_t pcr_ext = ((packet->data[offset + 4] & 0x01) << 8) |
                           packet->data[offset + 5];

        af->pcr = pcr_base * 300 + pcr_ext; /* 27MHz */
        offset += 6;
    }

    /* Parse OPCR */
    if (af->opcr_flag && (offset + 6) <= (4 + af->length)) {
        uint64_t opcr_base = ((uint64_t)packet->data[offset] << 25) |
                            ((uint64_t)packet->data[offset + 1] << 17) |
                            ((uint64_t)packet->data[offset + 2] << 9) |
                            ((uint64_t)packet->data[offset + 3] << 1) |
                            ((uint64_t)packet->data[offset + 4] >> 7);

        uint16_t opcr_ext = ((packet->data[offset + 4] & 0x01) << 8) |
                            packet->data[offset + 5];

        af->opcr = opcr_base * 300 + opcr_ext;
        offset += 6;
    }

    /* Parse splice countdown */
    if (af->splicing_flag && offset <= (4 + af->length)) {
        af->splice_countdown = (int8_t)packet->data[offset];
        offset++;
    }

    return 0;
}

int ts_extract_pcr(const ts_packet_raw_t *packet, uint64_t *pcr) {
    if (!packet || !pcr) return -1;

    ts_adaptation_field_t af;
    if (ts_parse_adaptation_field(packet, &af) < 0) {
        return -1;
    }

    if (!af.pcr_flag) {
        return -1;
    }

    *pcr = af.pcr;
    return 0;
}

void ts_create_null_packet(ts_packet_raw_t *packet) {
    if (!packet) return;

    memset(packet->data, 0xFF, TS_PACKET_SIZE);

    packet->data[0] = TS_SYNC_BYTE;
    packet->data[1] = 0x1F; /* PID 0x1FFF high byte */
    packet->data[2] = 0xFF; /* PID 0x1FFF low byte */
    packet->data[3] = 0x10; /* Payload only, CC = 0 */
}

void ts_stats_init(ts_stream_stats_t *stats) {
    if (!stats) return;

    memset(stats, 0, sizeof(ts_stream_stats_t));

    /* Initialize all PIDs with invalid last_cc */
    for (int i = 0; i < TS_MAX_PIDS; i++) {
        stats->pids[i].last_cc = 0xFF;
    }

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    stats->start_time = (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void ts_stats_update(ts_stream_stats_t *stats, const ts_packet_raw_t *packet) {
    if (!stats || !packet) return;

    /* Check sync byte */
    if (packet->data[0] != TS_SYNC_BYTE) {
        stats->sync_errors++;
        return;
    }

    stats->total_packets++;
    stats->total_bytes += TS_PACKET_SIZE;

    /* Get PID */
    uint16_t pid = ts_packet_get_pid(packet);
    ts_pid_stats_t *pid_stats = &stats->pids[pid];

    /* Update PID stats */
    if (pid_stats->packet_count == 0) {
        pid_stats->pid = pid;
        stats->pid_count++;
    }
    pid_stats->packet_count++;

    /* Check continuity counter */
    if (pid != TS_PID_NULL && ts_packet_has_payload(packet)) {
        uint8_t cc = ts_packet_get_cc(packet);
        if (pid_stats->last_cc != 0xFF) {
            uint8_t expected_cc = (pid_stats->last_cc + 1) & 0x0F;
            if (cc != expected_cc) {
                pid_stats->cc_errors++;
                stats->cc_errors++;
            }
        }
        pid_stats->last_cc = cc;
    }

    /* Check for PCR */
    uint64_t pcr;
    if (ts_extract_pcr(packet, &pcr) == 0) {
        if (pid_stats->last_pcr != 0 && pcr < pid_stats->last_pcr) {
            stats->pcr_discontinuities++;
        }
        pid_stats->last_pcr = pcr;
    }

    /* Update timestamp */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    stats->last_update = (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void ts_stats_calculate_bitrate(ts_stream_stats_t *stats) {
    if (!stats) return;

    uint64_t duration_ms = stats->last_update - stats->start_time;
    if (duration_ms > 0) {
        /* Bitrate in bps */
        stats->bitrate = (uint32_t)((stats->total_bytes * 8 * 1000) / duration_ms);
    }
}
