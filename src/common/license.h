/*
 * CariTranscoder - License Management System
 * Copyright (c) 2024 CariTech Solutions
 */

#ifndef CARI_LICENSE_H
#define CARI_LICENSE_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/* License types */
typedef enum {
    LICENSE_TYPE_FREE = 0,
    LICENSE_TYPE_BASIC,
    LICENSE_TYPE_PRO,
    LICENSE_TYPE_ENTERPRISE
} license_type_t;

/* License status */
typedef enum {
    LICENSE_STATUS_VALID = 0,
    LICENSE_STATUS_INVALID,
    LICENSE_STATUS_EXPIRED,
    LICENSE_STATUS_HARDWARE_MISMATCH,
    LICENSE_STATUS_NOT_FOUND,
    LICENSE_STATUS_TAMPERED
} license_status_t;

/* Feature flags */
typedef enum {
    LICENSE_FEATURE_TRANSCODE     = (1 << 0),
    LICENSE_FEATURE_GPU_ACCEL     = (1 << 1),
    LICENSE_FEATURE_HA_CLUSTER    = (1 << 2),
    LICENSE_FEATURE_TSDUCK_FULL   = (1 << 3),
    LICENSE_FEATURE_4K            = (1 << 4),
    LICENSE_FEATURE_HDR           = (1 << 5),
    LICENSE_FEATURE_SCTE35        = (1 << 6),
    LICENSE_FEATURE_API_ACCESS    = (1 << 7)
} license_feature_t;

/* Hardware fingerprint */
typedef struct {
    char machine_id[64];        /* /etc/machine-id */
    char mac_address[18];       /* Primary MAC address */
    char cpu_id[64];            /* CPU identifier */
    char disk_serial[64];       /* Primary disk serial */
    char fingerprint[128];      /* Combined hash */
} hardware_fingerprint_t;

/* License information */
typedef struct {
    /* Identification */
    char license_id[32];
    char customer_name[128];
    char customer_email[128];

    /* Type and status */
    license_type_t type;
    license_status_t status;

    /* Dates */
    time_t issued_date;
    time_t expiry_date;         /* 0 = perpetual */
    bool is_perpetual;

    /* Limits */
    int max_inputs;             /* 0 = unlimited */
    int max_outputs;
    int max_transcoders;
    int max_muxers;
    int max_nodes;              /* HA cluster nodes */

    /* Features */
    uint32_t features;          /* Bitmask of license_feature_t */

    /* Hardware binding */
    char hardware_fingerprint[128];
    bool hardware_locked;

    /* Internal */
    char signature[512];        /* RSA signature */
} license_info_t;

/* License limits for free tier */
#define LICENSE_FREE_MAX_INPUTS     2
#define LICENSE_FREE_MAX_OUTPUTS    2
#define LICENSE_FREE_MAX_TRANSCODERS 1
#define LICENSE_FREE_MAX_MUXERS     1
#define LICENSE_FREE_MAX_NODES      1

/**
 * Get hardware fingerprint of current system
 *
 * @param fp Output fingerprint structure
 * @return 0 on success, -1 on error
 */
int license_get_hardware_fingerprint(hardware_fingerprint_t *fp);

/**
 * Load license from file
 *
 * @param filename License file path
 * @param info Output license information
 * @return License status
 */
license_status_t license_load(const char *filename, license_info_t *info);

/**
 * Validate license
 *
 * @param info License information to validate
 * @return License status
 */
license_status_t license_validate(license_info_t *info);

/**
 * Check if a feature is enabled
 *
 * @param info License information
 * @param feature Feature to check
 * @return true if feature is enabled
 */
bool license_has_feature(const license_info_t *info, license_feature_t feature);

/**
 * Check if license allows more of a resource
 *
 * @param info License information
 * @param resource Resource type ("inputs", "outputs", "transcoders", "muxers", "nodes")
 * @param current Current count
 * @return true if more allowed
 */
bool license_can_add(const license_info_t *info, const char *resource, int current);

/**
 * Get maximum allowed for a resource
 *
 * @param info License information
 * @param resource Resource type
 * @return Maximum allowed (0 = unlimited)
 */
int license_get_max(const license_info_t *info, const char *resource);

/**
 * Get license type as string
 *
 * @param type License type
 * @return Type string
 */
const char* license_type_str(license_type_t type);

/**
 * Get license status as string
 *
 * @param status License status
 * @return Status string
 */
const char* license_status_str(license_status_t status);

/**
 * Get days until license expiry
 *
 * @param info License information
 * @return Days until expiry (-1 if perpetual, 0 if expired)
 */
int license_days_remaining(const license_info_t *info);

/**
 * Initialize with free license defaults
 *
 * @param info License information to initialize
 */
void license_init_free(license_info_t *info);

/**
 * Generate license request file
 *
 * @param filename Output filename
 * @param customer_name Customer name
 * @param customer_email Customer email
 * @return 0 on success, -1 on error
 */
int license_generate_request(const char *filename,
                              const char *customer_name,
                              const char *customer_email);

/**
 * Format license info for display
 *
 * @param info License information
 * @param buffer Output buffer
 * @param buflen Buffer length
 */
void license_format_info(const license_info_t *info, char *buffer, size_t buflen);

#endif /* CARI_LICENSE_H */
