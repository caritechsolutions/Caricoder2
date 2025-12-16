/*
 * CariTranscoder - MPEG-TS Packet Definitions
 * Copyright (c) 2024 CariTech Solutions
 */

#ifndef CARI_TS_PACKET_H
#define CARI_TS_PACKET_H

#include <stdint.h>
#include <stdbool.h>

/* TS packet constants */
#define TS_PACKET_SIZE          188
#define TS_SYNC_BYTE            0x47
#define TS_HEADER_SIZE          4

/* TS packet PID values */
#define TS_PID_PAT              0x0000
#define TS_PID_CAT              0x0001
#define TS_PID_TSDT             0x0002
#define TS_PID_NIT              0x0010
#define TS_PID_SDT              0x0011
#define TS_PID_EIT              0x0012
#define TS_PID_RST              0x0013
#define TS_PID_TDT              0x0014
#define TS_PID_NULL             0x1FFF

/* Maximum PIDs in a transport stream */
#define TS_MAX_PIDS             8192

/* Adaptation field flags */
#define TS_AF_DISCONTINUITY     0x80
#define TS_AF_RANDOM_ACCESS     0x40
#define TS_AF_ES_PRIORITY       0x20
#define TS_AF_PCR               0x10
#define TS_AF_OPCR              0x08
#define TS_AF_SPLICING          0x04
#define TS_AF_PRIVATE           0x02
#define TS_AF_EXTENSION         0x01

/* Stream types */
#define TS_STREAM_TYPE_MPEG1_VIDEO      0x01
#define TS_STREAM_TYPE_MPEG2_VIDEO      0x02
#define TS_STREAM_TYPE_MPEG1_AUDIO      0x03
#define TS_STREAM_TYPE_MPEG2_AUDIO      0x04
#define TS_STREAM_TYPE_PRIVATE_SECTIONS 0x05
#define TS_STREAM_TYPE_PRIVATE_DATA     0x06
#define TS_STREAM_TYPE_AAC_AUDIO        0x0F
#define TS_STREAM_TYPE_H264_VIDEO       0x1B
#define TS_STREAM_TYPE_H265_VIDEO       0x24
#define TS_STREAM_TYPE_AC3_AUDIO        0x81
#define TS_STREAM_TYPE_EAC3_AUDIO       0x87

/*
 * TS Packet structure (raw)
 * --------------------------
 * Byte 0:       Sync byte (0x47)
 * Byte 1-2:     Transport error | Payload unit start | Priority | PID
 * Byte 3:       Scrambling | Adaptation field | Continuity counter
 * Bytes 4-187:  Adaptation field and/or payload
 */
typedef struct __attribute__((packed)) {
    uint8_t data[TS_PACKET_SIZE];
} ts_packet_raw_t;

/*
 * Parsed TS packet header
 */
typedef struct {
    uint8_t  sync_byte;
    bool     transport_error;
    bool     payload_unit_start;
    bool     transport_priority;
    uint16_t pid;
    uint8_t  scrambling_control;
    uint8_t  adaptation_field_control;
    uint8_t  continuity_counter;
} ts_header_t;

/*
 * Adaptation field structure
 */
typedef struct {
    uint8_t  length;
    bool     discontinuity;
    bool     random_access;
    bool     es_priority;
    bool     pcr_flag;
    bool     opcr_flag;
    bool     splicing_flag;
    bool     private_data_flag;
    bool     extension_flag;
    uint64_t pcr;           /* 33-bit base + 9-bit extension (90kHz) */
    uint64_t opcr;
    int8_t   splice_countdown;
} ts_adaptation_field_t;

/*
 * Statistics for a single PID
 */
typedef struct {
    uint16_t pid;
    uint64_t packet_count;
    uint64_t cc_errors;
    uint8_t  last_cc;
    uint64_t last_pcr;
    uint32_t bitrate;       /* Calculated bitrate in bps */
    uint8_t  stream_type;
    bool     scrambled;
} ts_pid_stats_t;

/*
 * Overall stream statistics
 */
typedef struct {
    uint64_t total_packets;
    uint64_t total_bytes;
    uint64_t sync_errors;
    uint64_t cc_errors;
    uint64_t pcr_discontinuities;
    uint32_t bitrate;           /* Overall bitrate in bps */
    uint32_t pid_count;         /* Number of active PIDs */
    uint64_t start_time;        /* Timestamp when stats started */
    uint64_t last_update;       /* Last update timestamp */
    ts_pid_stats_t pids[TS_MAX_PIDS];
} ts_stream_stats_t;

/* Function prototypes */

/**
 * Validate TS packet sync byte
 * @param packet Raw TS packet
 * @return true if valid sync byte (0x47)
 */
static inline bool ts_packet_valid(const ts_packet_raw_t *packet) {
    return packet->data[0] == TS_SYNC_BYTE;
}

/**
 * Get PID from TS packet
 * @param packet Raw TS packet
 * @return 13-bit PID value
 */
static inline uint16_t ts_packet_get_pid(const ts_packet_raw_t *packet) {
    return ((packet->data[1] & 0x1F) << 8) | packet->data[2];
}

/**
 * Set PID in TS packet
 * @param packet Raw TS packet
 * @param pid New PID value
 */
static inline void ts_packet_set_pid(ts_packet_raw_t *packet, uint16_t pid) {
    packet->data[1] = (packet->data[1] & 0xE0) | ((pid >> 8) & 0x1F);
    packet->data[2] = pid & 0xFF;
}

/**
 * Get continuity counter from TS packet
 * @param packet Raw TS packet
 * @return 4-bit continuity counter
 */
static inline uint8_t ts_packet_get_cc(const ts_packet_raw_t *packet) {
    return packet->data[3] & 0x0F;
}

/**
 * Set continuity counter in TS packet
 * @param packet Raw TS packet
 * @param cc New continuity counter value
 */
static inline void ts_packet_set_cc(ts_packet_raw_t *packet, uint8_t cc) {
    packet->data[3] = (packet->data[3] & 0xF0) | (cc & 0x0F);
}

/**
 * Check if packet has adaptation field
 * @param packet Raw TS packet
 * @return true if adaptation field present
 */
static inline bool ts_packet_has_adaptation(const ts_packet_raw_t *packet) {
    return (packet->data[3] & 0x20) != 0;
}

/**
 * Check if packet has payload
 * @param packet Raw TS packet
 * @return true if payload present
 */
static inline bool ts_packet_has_payload(const ts_packet_raw_t *packet) {
    return (packet->data[3] & 0x10) != 0;
}

/**
 * Check if packet is payload unit start
 * @param packet Raw TS packet
 * @return true if payload unit start indicator set
 */
static inline bool ts_packet_is_pusi(const ts_packet_raw_t *packet) {
    return (packet->data[1] & 0x40) != 0;
}

/**
 * Get adaptation field length
 * @param packet Raw TS packet
 * @return Adaptation field length (0 if no adaptation field)
 */
static inline uint8_t ts_packet_get_af_length(const ts_packet_raw_t *packet) {
    if (!ts_packet_has_adaptation(packet)) {
        return 0;
    }
    return packet->data[4];
}

/**
 * Get pointer to payload data
 * @param packet Raw TS packet
 * @param payload_size Output parameter for payload size
 * @return Pointer to payload data, or NULL if no payload
 */
static inline uint8_t* ts_packet_get_payload(ts_packet_raw_t *packet,
                                              uint8_t *payload_size) {
    if (!ts_packet_has_payload(packet)) {
        if (payload_size) *payload_size = 0;
        return NULL;
    }

    uint8_t offset = TS_HEADER_SIZE;
    if (ts_packet_has_adaptation(packet)) {
        offset += 1 + packet->data[4];
    }

    if (payload_size) {
        *payload_size = TS_PACKET_SIZE - offset;
    }

    return &packet->data[offset];
}

/**
 * Parse full TS header
 * @param packet Raw TS packet
 * @param header Output header structure
 * @return 0 on success, -1 on error
 */
int ts_parse_header(const ts_packet_raw_t *packet, ts_header_t *header);

/**
 * Parse adaptation field
 * @param packet Raw TS packet
 * @param af Output adaptation field structure
 * @return 0 on success, -1 on error or no adaptation field
 */
int ts_parse_adaptation_field(const ts_packet_raw_t *packet,
                               ts_adaptation_field_t *af);

/**
 * Extract PCR from adaptation field
 * @param packet Raw TS packet
 * @param pcr Output PCR value (27MHz clock)
 * @return 0 on success, -1 if no PCR
 */
int ts_extract_pcr(const ts_packet_raw_t *packet, uint64_t *pcr);

/**
 * Create null packet
 * @param packet Output packet (will be filled with null packet)
 */
void ts_create_null_packet(ts_packet_raw_t *packet);

/**
 * Initialize stream statistics
 * @param stats Statistics structure to initialize
 */
void ts_stats_init(ts_stream_stats_t *stats);

/**
 * Update statistics with packet
 * @param stats Statistics structure
 * @param packet TS packet to process
 */
void ts_stats_update(ts_stream_stats_t *stats, const ts_packet_raw_t *packet);

/**
 * Calculate bitrate from statistics
 * @param stats Statistics structure
 */
void ts_stats_calculate_bitrate(ts_stream_stats_t *stats);

#endif /* CARI_TS_PACKET_H */
