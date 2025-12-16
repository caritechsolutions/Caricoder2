/*
 * CariTranscoder - Lock-Free Ring Buffer for TS Packets
 * Copyright (c) 2024 CariTech Solutions
 *
 * Shared memory ring buffer for inter-process communication.
 * Uses lock-free atomic operations for thread/process safety.
 */

#ifndef CARI_RING_BUFFER_H
#define CARI_RING_BUFFER_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include "ts_packet.h"

/* Magic number for buffer validation */
#define RING_BUFFER_MAGIC       0x54535242  /* "TSRB" */
#define RING_BUFFER_VERSION     1

/* Default buffer size (number of TS packets) */
#define RING_BUFFER_DEFAULT_PACKETS     557000  /* ~100MB */

/* Buffer status flags */
#define RING_BUFFER_FLAG_ACTIVE         0x0001
#define RING_BUFFER_FLAG_WRITER_PRESENT 0x0002
#define RING_BUFFER_FLAG_READER_PRESENT 0x0004
#define RING_BUFFER_FLAG_OVERFLOW       0x0008
#define RING_BUFFER_FLAG_UNDERFLOW      0x0010
#define RING_BUFFER_FLAG_ERROR          0x8000

/* Overflow policies */
typedef enum {
    RING_BUFFER_OVERFLOW_DROP_OLDEST,   /* Drop oldest packets (live streaming) */
    RING_BUFFER_OVERFLOW_BLOCK_WRITER,  /* Block writer until space available */
    RING_BUFFER_OVERFLOW_DROP_NEWEST    /* Drop incoming packets */
} ring_buffer_overflow_policy_t;

/*
 * Ring buffer header structure
 * This is placed at the start of the shared memory region
 */
typedef struct {
    /* Identification */
    uint32_t magic;                         /* RING_BUFFER_MAGIC */
    uint32_t version;                       /* Buffer version */
    char     name[64];                      /* Buffer name */

    /* Configuration */
    uint32_t capacity;                      /* Number of TS packets */
    uint32_t packet_size;                   /* Size of each packet (188) */
    ring_buffer_overflow_policy_t overflow_policy;

    /* Indexes (atomic for lock-free operation) */
    _Atomic uint32_t write_idx;             /* Next write position */
    _Atomic uint32_t read_idx;              /* Next read position */

    /* Status */
    _Atomic uint32_t flags;                 /* Status flags */
    _Atomic uint64_t writer_heartbeat;      /* Writer last activity timestamp */
    _Atomic uint64_t reader_heartbeat;      /* Reader last activity timestamp */

    /* Statistics */
    _Atomic uint64_t total_written;         /* Total packets written */
    _Atomic uint64_t total_read;            /* Total packets read */
    _Atomic uint64_t overruns;              /* Buffer overrun count */
    _Atomic uint64_t underruns;             /* Buffer underrun count */

    /* Writer/Reader identification */
    pid_t writer_pid;                       /* Writer process ID */
    pid_t reader_pid;                       /* Reader process ID */

    /* Padding to cache line boundary */
    uint8_t _padding[64];

    /* Data starts here - packets array */
    /* ts_packet_raw_t packets[]; */
} ring_buffer_header_t;

/*
 * Ring buffer handle (per-process)
 */
typedef struct {
    ring_buffer_header_t *header;           /* Pointer to shared header */
    ts_packet_raw_t *packets;               /* Pointer to packet array */
    int shm_fd;                             /* Shared memory file descriptor */
    size_t shm_size;                        /* Total shared memory size */
    char shm_name[128];                     /* Shared memory name */
    bool is_writer;                         /* true if this is the writer */
    bool is_creator;                        /* true if this process created the buffer */
} ring_buffer_t;

/*
 * Buffer creation options
 */
typedef struct {
    uint32_t capacity;                      /* Number of packets (0 = default) */
    ring_buffer_overflow_policy_t overflow_policy;
    bool create;                            /* Create if doesn't exist */
    bool exclusive;                         /* Fail if already exists */
    mode_t mode;                            /* Permissions (default 0660) */
} ring_buffer_options_t;

/* Default options initializer */
#define RING_BUFFER_OPTIONS_DEFAULT { \
    .capacity = RING_BUFFER_DEFAULT_PACKETS, \
    .overflow_policy = RING_BUFFER_OVERFLOW_DROP_OLDEST, \
    .create = true, \
    .exclusive = false, \
    .mode = 0660 \
}

/**
 * Create or open a ring buffer
 *
 * @param name Buffer name (will be prefixed with /caritrans_)
 * @param options Buffer options (NULL for defaults)
 * @param is_writer true if this process will write to the buffer
 * @return Ring buffer handle, or NULL on error
 */
ring_buffer_t* ring_buffer_open(const char *name,
                                 const ring_buffer_options_t *options,
                                 bool is_writer);

/**
 * Close ring buffer and release resources
 *
 * @param rb Ring buffer handle
 * @param unlink true to remove shared memory (only creator should do this)
 */
void ring_buffer_close(ring_buffer_t *rb, bool unlink);

/**
 * Write a single TS packet to the buffer
 *
 * @param rb Ring buffer handle
 * @param packet Packet to write
 * @return 0 on success, -1 on error, 1 if dropped due to overflow
 */
int ring_buffer_write(ring_buffer_t *rb, const ts_packet_raw_t *packet);

/**
 * Write multiple TS packets to the buffer
 *
 * @param rb Ring buffer handle
 * @param packets Array of packets to write
 * @param count Number of packets
 * @return Number of packets written, -1 on error
 */
int ring_buffer_write_batch(ring_buffer_t *rb,
                            const ts_packet_raw_t *packets,
                            uint32_t count);

/**
 * Read a single TS packet from the buffer
 *
 * @param rb Ring buffer handle
 * @param packet Output packet buffer
 * @return 0 on success, -1 on error, 1 if buffer empty
 */
int ring_buffer_read(ring_buffer_t *rb, ts_packet_raw_t *packet);

/**
 * Read multiple TS packets from the buffer
 *
 * @param rb Ring buffer handle
 * @param packets Output packet buffer array
 * @param max_count Maximum packets to read
 * @return Number of packets read, -1 on error
 */
int ring_buffer_read_batch(ring_buffer_t *rb,
                           ts_packet_raw_t *packets,
                           uint32_t max_count);

/**
 * Peek at next packet without removing from buffer
 *
 * @param rb Ring buffer handle
 * @param packet Output packet buffer
 * @return 0 on success, -1 on error, 1 if buffer empty
 */
int ring_buffer_peek(ring_buffer_t *rb, ts_packet_raw_t *packet);

/**
 * Get number of packets available to read
 *
 * @param rb Ring buffer handle
 * @return Number of packets available
 */
uint32_t ring_buffer_available(ring_buffer_t *rb);

/**
 * Get free space in packets
 *
 * @param rb Ring buffer handle
 * @return Number of free packet slots
 */
uint32_t ring_buffer_free_space(ring_buffer_t *rb);

/**
 * Check if buffer is empty
 *
 * @param rb Ring buffer handle
 * @return true if empty
 */
bool ring_buffer_empty(ring_buffer_t *rb);

/**
 * Check if buffer is full
 *
 * @param rb Ring buffer handle
 * @return true if full
 */
bool ring_buffer_full(ring_buffer_t *rb);

/**
 * Flush/clear the buffer
 *
 * @param rb Ring buffer handle
 */
void ring_buffer_flush(ring_buffer_t *rb);

/**
 * Update heartbeat (should be called periodically)
 *
 * @param rb Ring buffer handle
 */
void ring_buffer_heartbeat(ring_buffer_t *rb);

/**
 * Check if writer is alive (based on heartbeat)
 *
 * @param rb Ring buffer handle
 * @param timeout_ms Heartbeat timeout in milliseconds
 * @return true if writer is alive
 */
bool ring_buffer_writer_alive(ring_buffer_t *rb, uint32_t timeout_ms);

/**
 * Check if reader is alive (based on heartbeat)
 *
 * @param rb Ring buffer handle
 * @param timeout_ms Heartbeat timeout in milliseconds
 * @return true if reader is alive
 */
bool ring_buffer_reader_alive(ring_buffer_t *rb, uint32_t timeout_ms);

/**
 * Get buffer statistics
 *
 * @param rb Ring buffer handle
 * @param total_written Output: total packets written
 * @param total_read Output: total packets read
 * @param overruns Output: overrun count
 * @param underruns Output: underrun count
 */
void ring_buffer_get_stats(ring_buffer_t *rb,
                           uint64_t *total_written,
                           uint64_t *total_read,
                           uint64_t *overruns,
                           uint64_t *underruns);

/**
 * Get buffer fill level as percentage
 *
 * @param rb Ring buffer handle
 * @return Fill percentage (0-100)
 */
uint8_t ring_buffer_fill_percent(ring_buffer_t *rb);

/**
 * Set error flag on buffer
 *
 * @param rb Ring buffer handle
 * @param error_flag Error flag to set
 */
void ring_buffer_set_error(ring_buffer_t *rb, uint32_t error_flag);

/**
 * Clear error flag on buffer
 *
 * @param rb Ring buffer handle
 */
void ring_buffer_clear_error(ring_buffer_t *rb);

/**
 * Check if buffer has error
 *
 * @param rb Ring buffer handle
 * @return true if error flag is set
 */
bool ring_buffer_has_error(ring_buffer_t *rb);

#endif /* CARI_RING_BUFFER_H */
