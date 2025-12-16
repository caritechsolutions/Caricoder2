/*
 * CariTranscoder - Lock-Free Ring Buffer Implementation
 * Copyright (c) 2024 CariTech Solutions
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "ring_buffer.h"
#include "logging.h"

/* Shared memory name prefix */
#define SHM_PREFIX "/caritrans_"

/* Get current timestamp in milliseconds */
static uint64_t get_timestamp_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Calculate total shared memory size */
static size_t calculate_shm_size(uint32_t capacity) {
    return sizeof(ring_buffer_header_t) + (capacity * sizeof(ts_packet_raw_t));
}

ring_buffer_t* ring_buffer_open(const char *name,
                                 const ring_buffer_options_t *options,
                                 bool is_writer) {
    ring_buffer_t *rb = NULL;
    ring_buffer_options_t opts;
    int shm_fd = -1;
    void *shm_ptr = MAP_FAILED;
    bool created = false;

    /* Use defaults if no options provided */
    if (options) {
        opts = *options;
    } else {
        ring_buffer_options_t default_opts = RING_BUFFER_OPTIONS_DEFAULT;
        opts = default_opts;
    }

    /* Validate capacity */
    if (opts.capacity == 0) {
        opts.capacity = RING_BUFFER_DEFAULT_PACKETS;
    }

    /* Allocate handle */
    rb = calloc(1, sizeof(ring_buffer_t));
    if (!rb) {
        LOG_ERROR("Failed to allocate ring buffer handle");
        return NULL;
    }

    /* Build shared memory name */
    snprintf(rb->shm_name, sizeof(rb->shm_name), "%s%s", SHM_PREFIX, name);

    /* Calculate size */
    rb->shm_size = calculate_shm_size(opts.capacity);

    /* Try to open existing or create new */
    int flags = O_RDWR;
    if (opts.create) {
        flags |= O_CREAT;
    }
    if (opts.exclusive) {
        flags |= O_EXCL;
    }

    shm_fd = shm_open(rb->shm_name, flags, opts.mode);
    if (shm_fd < 0) {
        if (errno == EEXIST && opts.exclusive) {
            LOG_ERROR("Ring buffer '%s' already exists", name);
        } else if (errno == ENOENT && !opts.create) {
            LOG_ERROR("Ring buffer '%s' does not exist", name);
        } else {
            LOG_ERROR("Failed to open shared memory '%s': %s",
                      rb->shm_name, strerror(errno));
        }
        goto error;
    }

    /* Check if we created it */
    struct stat st;
    if (fstat(shm_fd, &st) == 0 && st.st_size == 0) {
        created = true;
        /* Set size */
        if (ftruncate(shm_fd, rb->shm_size) < 0) {
            LOG_ERROR("Failed to set shared memory size: %s", strerror(errno));
            goto error;
        }
    }

    /* Map shared memory */
    shm_ptr = mmap(NULL, rb->shm_size, PROT_READ | PROT_WRITE,
                   MAP_SHARED, shm_fd, 0);
    if (shm_ptr == MAP_FAILED) {
        LOG_ERROR("Failed to map shared memory: %s", strerror(errno));
        goto error;
    }

    /* Setup handle */
    rb->shm_fd = shm_fd;
    rb->header = (ring_buffer_header_t*)shm_ptr;
    rb->packets = (ts_packet_raw_t*)((uint8_t*)shm_ptr + sizeof(ring_buffer_header_t));
    rb->is_writer = is_writer;
    rb->is_creator = created;

    /* Initialize header if we created it */
    if (created) {
        memset(rb->header, 0, sizeof(ring_buffer_header_t));
        rb->header->magic = RING_BUFFER_MAGIC;
        rb->header->version = RING_BUFFER_VERSION;
        strncpy(rb->header->name, name, sizeof(rb->header->name) - 1);
        rb->header->capacity = opts.capacity;
        rb->header->packet_size = TS_PACKET_SIZE;
        rb->header->overflow_policy = opts.overflow_policy;
        atomic_store(&rb->header->write_idx, 0);
        atomic_store(&rb->header->read_idx, 0);
        atomic_store(&rb->header->flags, RING_BUFFER_FLAG_ACTIVE);
        LOG_INFO("Created ring buffer '%s' with capacity %u packets (%.1f MB)",
                 name, opts.capacity,
                 (double)rb->shm_size / (1024 * 1024));
    } else {
        /* Validate existing buffer */
        if (rb->header->magic != RING_BUFFER_MAGIC) {
            LOG_ERROR("Invalid ring buffer magic number");
            goto error;
        }
        if (rb->header->version != RING_BUFFER_VERSION) {
            LOG_ERROR("Ring buffer version mismatch (expected %u, got %u)",
                      RING_BUFFER_VERSION, rb->header->version);
            goto error;
        }
        LOG_INFO("Opened existing ring buffer '%s'", name);
    }

    /* Register as writer or reader */
    if (is_writer) {
        rb->header->writer_pid = getpid();
        atomic_fetch_or(&rb->header->flags, RING_BUFFER_FLAG_WRITER_PRESENT);
    } else {
        rb->header->reader_pid = getpid();
        atomic_fetch_or(&rb->header->flags, RING_BUFFER_FLAG_READER_PRESENT);
    }

    /* Initial heartbeat */
    ring_buffer_heartbeat(rb);

    return rb;

error:
    if (shm_ptr != MAP_FAILED) {
        munmap(shm_ptr, rb->shm_size);
    }
    if (shm_fd >= 0) {
        close(shm_fd);
        if (created) {
            shm_unlink(rb->shm_name);
        }
    }
    free(rb);
    return NULL;
}

void ring_buffer_close(ring_buffer_t *rb, bool unlink) {
    if (!rb) return;

    /* Clear presence flag */
    if (rb->is_writer) {
        atomic_fetch_and(&rb->header->flags, ~RING_BUFFER_FLAG_WRITER_PRESENT);
        rb->header->writer_pid = 0;
    } else {
        atomic_fetch_and(&rb->header->flags, ~RING_BUFFER_FLAG_READER_PRESENT);
        rb->header->reader_pid = 0;
    }

    /* Unmap */
    if (rb->header) {
        munmap(rb->header, rb->shm_size);
    }

    /* Close fd */
    if (rb->shm_fd >= 0) {
        close(rb->shm_fd);
    }

    /* Unlink if requested */
    if (unlink) {
        shm_unlink(rb->shm_name);
        LOG_INFO("Unlinked ring buffer '%s'", rb->shm_name);
    }

    free(rb);
}

int ring_buffer_write(ring_buffer_t *rb, const ts_packet_raw_t *packet) {
    if (!rb || !rb->is_writer || !packet) {
        return -1;
    }

    uint32_t write_idx = atomic_load(&rb->header->write_idx);
    uint32_t read_idx = atomic_load(&rb->header->read_idx);
    uint32_t next_write = (write_idx + 1) % rb->header->capacity;

    /* Check for full buffer */
    if (next_write == read_idx) {
        switch (rb->header->overflow_policy) {
            case RING_BUFFER_OVERFLOW_DROP_OLDEST:
                /* Advance read pointer to make space */
                atomic_compare_exchange_weak(&rb->header->read_idx,
                                             &read_idx,
                                             (read_idx + 1) % rb->header->capacity);
                atomic_fetch_add(&rb->header->overruns, 1);
                atomic_fetch_or(&rb->header->flags, RING_BUFFER_FLAG_OVERFLOW);
                break;

            case RING_BUFFER_OVERFLOW_DROP_NEWEST:
                atomic_fetch_add(&rb->header->overruns, 1);
                return 1; /* Packet dropped */

            case RING_BUFFER_OVERFLOW_BLOCK_WRITER:
                /* Spin wait for space (with timeout) */
                for (int i = 0; i < 1000; i++) {
                    read_idx = atomic_load(&rb->header->read_idx);
                    if (next_write != read_idx) break;
                    usleep(100);
                }
                if (next_write == read_idx) {
                    atomic_fetch_add(&rb->header->overruns, 1);
                    return 1; /* Timeout, drop packet */
                }
                break;
        }
    }

    /* Copy packet */
    memcpy(&rb->packets[write_idx], packet, sizeof(ts_packet_raw_t));

    /* Update write index */
    atomic_store(&rb->header->write_idx, next_write);
    atomic_fetch_add(&rb->header->total_written, 1);

    return 0;
}

int ring_buffer_write_batch(ring_buffer_t *rb,
                            const ts_packet_raw_t *packets,
                            uint32_t count) {
    if (!rb || !rb->is_writer || !packets || count == 0) {
        return -1;
    }

    int written = 0;
    for (uint32_t i = 0; i < count; i++) {
        int ret = ring_buffer_write(rb, &packets[i]);
        if (ret < 0) return -1;
        if (ret == 0) written++;
    }

    return written;
}

int ring_buffer_read(ring_buffer_t *rb, ts_packet_raw_t *packet) {
    if (!rb || !packet) {
        return -1;
    }

    uint32_t write_idx = atomic_load(&rb->header->write_idx);
    uint32_t read_idx = atomic_load(&rb->header->read_idx);

    /* Check for empty buffer */
    if (read_idx == write_idx) {
        atomic_fetch_add(&rb->header->underruns, 1);
        atomic_fetch_or(&rb->header->flags, RING_BUFFER_FLAG_UNDERFLOW);
        return 1; /* Buffer empty */
    }

    /* Copy packet */
    memcpy(packet, &rb->packets[read_idx], sizeof(ts_packet_raw_t));

    /* Update read index */
    atomic_store(&rb->header->read_idx, (read_idx + 1) % rb->header->capacity);
    atomic_fetch_add(&rb->header->total_read, 1);

    /* Clear underflow flag if we have data */
    atomic_fetch_and(&rb->header->flags, ~RING_BUFFER_FLAG_UNDERFLOW);

    return 0;
}

int ring_buffer_read_batch(ring_buffer_t *rb,
                           ts_packet_raw_t *packets,
                           uint32_t max_count) {
    if (!rb || !packets || max_count == 0) {
        return -1;
    }

    int read_count = 0;
    for (uint32_t i = 0; i < max_count; i++) {
        int ret = ring_buffer_read(rb, &packets[i]);
        if (ret < 0) return -1;
        if (ret > 0) break; /* Buffer empty */
        read_count++;
    }

    return read_count;
}

int ring_buffer_peek(ring_buffer_t *rb, ts_packet_raw_t *packet) {
    if (!rb || !packet) {
        return -1;
    }

    uint32_t write_idx = atomic_load(&rb->header->write_idx);
    uint32_t read_idx = atomic_load(&rb->header->read_idx);

    if (read_idx == write_idx) {
        return 1; /* Buffer empty */
    }

    memcpy(packet, &rb->packets[read_idx], sizeof(ts_packet_raw_t));
    return 0;
}

uint32_t ring_buffer_available(ring_buffer_t *rb) {
    if (!rb) return 0;

    uint32_t write_idx = atomic_load(&rb->header->write_idx);
    uint32_t read_idx = atomic_load(&rb->header->read_idx);

    if (write_idx >= read_idx) {
        return write_idx - read_idx;
    } else {
        return rb->header->capacity - read_idx + write_idx;
    }
}

uint32_t ring_buffer_free_space(ring_buffer_t *rb) {
    if (!rb) return 0;
    return rb->header->capacity - ring_buffer_available(rb) - 1;
}

bool ring_buffer_empty(ring_buffer_t *rb) {
    if (!rb) return true;
    return atomic_load(&rb->header->write_idx) == atomic_load(&rb->header->read_idx);
}

bool ring_buffer_full(ring_buffer_t *rb) {
    if (!rb) return false;
    uint32_t write_idx = atomic_load(&rb->header->write_idx);
    uint32_t read_idx = atomic_load(&rb->header->read_idx);
    return ((write_idx + 1) % rb->header->capacity) == read_idx;
}

void ring_buffer_flush(ring_buffer_t *rb) {
    if (!rb) return;
    atomic_store(&rb->header->read_idx, atomic_load(&rb->header->write_idx));
}

void ring_buffer_heartbeat(ring_buffer_t *rb) {
    if (!rb) return;

    uint64_t now = get_timestamp_ms();
    if (rb->is_writer) {
        atomic_store(&rb->header->writer_heartbeat, now);
    } else {
        atomic_store(&rb->header->reader_heartbeat, now);
    }
}

bool ring_buffer_writer_alive(ring_buffer_t *rb, uint32_t timeout_ms) {
    if (!rb) return false;

    uint64_t now = get_timestamp_ms();
    uint64_t last = atomic_load(&rb->header->writer_heartbeat);

    return (now - last) < timeout_ms;
}

bool ring_buffer_reader_alive(ring_buffer_t *rb, uint32_t timeout_ms) {
    if (!rb) return false;

    uint64_t now = get_timestamp_ms();
    uint64_t last = atomic_load(&rb->header->reader_heartbeat);

    return (now - last) < timeout_ms;
}

void ring_buffer_get_stats(ring_buffer_t *rb,
                           uint64_t *total_written,
                           uint64_t *total_read,
                           uint64_t *overruns,
                           uint64_t *underruns) {
    if (!rb) return;

    if (total_written) *total_written = atomic_load(&rb->header->total_written);
    if (total_read) *total_read = atomic_load(&rb->header->total_read);
    if (overruns) *overruns = atomic_load(&rb->header->overruns);
    if (underruns) *underruns = atomic_load(&rb->header->underruns);
}

uint8_t ring_buffer_fill_percent(ring_buffer_t *rb) {
    if (!rb || rb->header->capacity == 0) return 0;
    return (uint8_t)((ring_buffer_available(rb) * 100) / rb->header->capacity);
}

void ring_buffer_set_error(ring_buffer_t *rb, uint32_t error_flag) {
    if (!rb) return;
    atomic_fetch_or(&rb->header->flags, error_flag | RING_BUFFER_FLAG_ERROR);
}

void ring_buffer_clear_error(ring_buffer_t *rb) {
    if (!rb) return;
    atomic_fetch_and(&rb->header->flags, ~RING_BUFFER_FLAG_ERROR);
}

bool ring_buffer_has_error(ring_buffer_t *rb) {
    if (!rb) return false;
    return (atomic_load(&rb->header->flags) & RING_BUFFER_FLAG_ERROR) != 0;
}
