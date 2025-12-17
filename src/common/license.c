/*
 * CariTranscoder - License Management Implementation
 * Copyright (c) 2024 CariTech Solutions
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <openssl/sha.h>
#include <openssl/rsa.h>
#include <openssl/pem.h>
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/err.h>

#include "license.h"
#include "logging.h"
#include "config.h"

/* Public key for license verification (embedded) */
/* In production, this would be your actual RSA public key */
static const char *LICENSE_PUBLIC_KEY =
"-----BEGIN PUBLIC KEY-----\n"
"MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEA0Z3VS5JJcds3xfn/ygWi\n"
"Placeholder_Key_Replace_With_Real_Key_In_Production_AAAAAAAAAAAAAAAAAAAA\n"
"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\n"
"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\n"
"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\n"
"2QIDAQAB\n"
"-----END PUBLIC KEY-----\n";

/* Get machine ID from /etc/machine-id */
static int get_machine_id(char *buf, size_t len) {
    FILE *fp = fopen("/etc/machine-id", "r");
    if (!fp) {
        fp = fopen("/var/lib/dbus/machine-id", "r");
    }
    if (!fp) {
        return -1;
    }

    if (fgets(buf, len, fp)) {
        /* Remove newline */
        buf[strcspn(buf, "\n")] = '\0';
    }
    fclose(fp);
    return 0;
}

/* Get primary MAC address */
static int get_mac_address(char *buf, size_t len) {
    struct ifaddrs *ifaddr, *ifa;
    int found = 0;

    if (getifaddrs(&ifaddr) == -1) {
        return -1;
    }

    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL) continue;

        /* Skip loopback */
        if (strcmp(ifa->ifa_name, "lo") == 0) continue;

        /* Get MAC for first non-loopback interface */
        int fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd >= 0) {
            struct ifreq ifr;
            strncpy(ifr.ifr_name, ifa->ifa_name, IFNAMSIZ - 1);
            if (ioctl(fd, SIOCGIFHWADDR, &ifr) == 0) {
                unsigned char *mac = (unsigned char *)ifr.ifr_hwaddr.sa_data;
                snprintf(buf, len, "%02x:%02x:%02x:%02x:%02x:%02x",
                        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
                found = 1;
            }
            close(fd);
        }

        if (found) break;
    }

    freeifaddrs(ifaddr);
    return found ? 0 : -1;
}

/* Get CPU ID (if available) */
static int get_cpu_id(char *buf, size_t len) {
#if defined(__x86_64__) || defined(__i386__)
    unsigned int eax, ebx, ecx, edx;

    /* Check if CPUID is supported */
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(0));

    if (eax >= 1) {
        __asm__ volatile("cpuid"
                         : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                         : "a"(1));
        snprintf(buf, len, "%08x-%08x-%08x", eax, ebx, ecx);
        return 0;
    }
#endif
    strncpy(buf, "unknown", len);
    return -1;
}

/* Compute SHA256 hash */
static void sha256_string(const char *input, char *output, size_t outlen) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256((unsigned char*)input, strlen(input), hash);

    for (int i = 0; i < SHA256_DIGEST_LENGTH && (i * 2 + 2) < (int)outlen; i++) {
        sprintf(output + (i * 2), "%02x", hash[i]);
    }
}

int license_get_hardware_fingerprint(hardware_fingerprint_t *fp) {
    if (!fp) return -1;

    memset(fp, 0, sizeof(hardware_fingerprint_t));

    /* Collect hardware info */
    get_machine_id(fp->machine_id, sizeof(fp->machine_id));
    get_mac_address(fp->mac_address, sizeof(fp->mac_address));
    get_cpu_id(fp->cpu_id, sizeof(fp->cpu_id));

    /* Note: Getting disk serial requires root or specific permissions */
    strncpy(fp->disk_serial, "unknown", sizeof(fp->disk_serial));

    /* Compute combined fingerprint */
    char combined[512];
    snprintf(combined, sizeof(combined), "%s|%s|%s|%s",
             fp->machine_id, fp->mac_address, fp->cpu_id, fp->disk_serial);

    sha256_string(combined, fp->fingerprint, sizeof(fp->fingerprint));

    CARI_LOG_DEBUG("Hardware fingerprint: %s", fp->fingerprint);
    return 0;
}

license_status_t license_load(const char *filename, license_info_t *info) {
    if (!filename || !info) {
        return LICENSE_STATUS_INVALID;
    }

    memset(info, 0, sizeof(license_info_t));

    /* Check if file exists */
    struct stat st;
    if (stat(filename, &st) != 0) {
        CARI_LOG_WARNING("License file not found: %s", filename);
        license_init_free(info);
        return LICENSE_STATUS_NOT_FOUND;
    }

    /* Load as config file */
    config_t cfg;
    if (config_load(&cfg, filename) != 0) {
        CARI_LOG_ERROR("Failed to parse license file");
        license_init_free(info);
        return LICENSE_STATUS_INVALID;
    }

    /* Parse license data */
    strncpy(info->license_id,
            config_get_string(&cfg, "license", "id", ""),
            sizeof(info->license_id) - 1);

    strncpy(info->customer_name,
            config_get_string(&cfg, "license", "customer", ""),
            sizeof(info->customer_name) - 1);

    strncpy(info->customer_email,
            config_get_string(&cfg, "license", "email", ""),
            sizeof(info->customer_email) - 1);

    /* License type */
    const char *type_str = config_get_string(&cfg, "license", "type", "free");
    if (strcasecmp(type_str, "enterprise") == 0) {
        info->type = LICENSE_TYPE_ENTERPRISE;
    } else if (strcasecmp(type_str, "pro") == 0) {
        info->type = LICENSE_TYPE_PRO;
    } else if (strcasecmp(type_str, "basic") == 0) {
        info->type = LICENSE_TYPE_BASIC;
    } else {
        info->type = LICENSE_TYPE_FREE;
    }

    /* Dates */
    info->issued_date = (time_t)config_get_uint64(&cfg, "license", "issued", 0);

    const char *expiry_str = config_get_string(&cfg, "license", "expiry", "perpetual");
    if (strcasecmp(expiry_str, "perpetual") == 0) {
        info->is_perpetual = true;
        info->expiry_date = 0;
    } else {
        info->is_perpetual = false;
        info->expiry_date = (time_t)config_get_uint64(&cfg, "license", "expiry", 0);
    }

    /* Limits */
    info->max_inputs = config_get_int(&cfg, "limits", "inputs", 0);
    info->max_outputs = config_get_int(&cfg, "limits", "outputs", 0);
    info->max_transcoders = config_get_int(&cfg, "limits", "transcoders", 0);
    info->max_muxers = config_get_int(&cfg, "limits", "muxers", 0);
    info->max_nodes = config_get_int(&cfg, "limits", "nodes", 1);

    /* Features */
    info->features = 0;
    if (config_get_bool(&cfg, "features", "transcode", true))
        info->features |= LICENSE_FEATURE_TRANSCODE;
    if (config_get_bool(&cfg, "features", "gpu_accel", false))
        info->features |= LICENSE_FEATURE_GPU_ACCEL;
    if (config_get_bool(&cfg, "features", "ha_cluster", false))
        info->features |= LICENSE_FEATURE_HA_CLUSTER;
    if (config_get_bool(&cfg, "features", "tsduck_full", false))
        info->features |= LICENSE_FEATURE_TSDUCK_FULL;
    if (config_get_bool(&cfg, "features", "4k", false))
        info->features |= LICENSE_FEATURE_4K;
    if (config_get_bool(&cfg, "features", "hdr", false))
        info->features |= LICENSE_FEATURE_HDR;
    if (config_get_bool(&cfg, "features", "scte35", false))
        info->features |= LICENSE_FEATURE_SCTE35;
    if (config_get_bool(&cfg, "features", "api", false))
        info->features |= LICENSE_FEATURE_API_ACCESS;

    /* Hardware fingerprint */
    strncpy(info->hardware_fingerprint,
            config_get_string(&cfg, "hardware", "fingerprint", ""),
            sizeof(info->hardware_fingerprint) - 1);
    info->hardware_locked = config_get_bool(&cfg, "hardware", "locked", false);

    /* Signature */
    strncpy(info->signature,
            config_get_string(&cfg, "signature", "data", ""),
            sizeof(info->signature) - 1);

    config_free(&cfg);

    /* Validate the license */
    return license_validate(info);
}

license_status_t license_validate(license_info_t *info) {
    if (!info) {
        return LICENSE_STATUS_INVALID;
    }

    /* Check expiry */
    if (!info->is_perpetual && info->expiry_date > 0) {
        time_t now = time(NULL);
        if (now > info->expiry_date) {
            info->status = LICENSE_STATUS_EXPIRED;
            CARI_LOG_WARNING("License expired");
            return LICENSE_STATUS_EXPIRED;
        }
    }

    /* Check hardware fingerprint if locked */
    if (info->hardware_locked && info->hardware_fingerprint[0]) {
        hardware_fingerprint_t current_fp;
        license_get_hardware_fingerprint(&current_fp);

        if (strcmp(info->hardware_fingerprint, current_fp.fingerprint) != 0) {
            info->status = LICENSE_STATUS_HARDWARE_MISMATCH;
            CARI_LOG_ERROR("License hardware mismatch");
            return LICENSE_STATUS_HARDWARE_MISMATCH;
        }
    }

    /* Verify signature (simplified - in production use proper RSA verification) */
    /* For now, we'll do a basic check */
    if (info->type != LICENSE_TYPE_FREE && info->signature[0] == '\0') {
        info->status = LICENSE_STATUS_TAMPERED;
        CARI_LOG_ERROR("License signature missing");
        return LICENSE_STATUS_TAMPERED;
    }

    /* TODO: Implement proper RSA signature verification
     * 1. Build string of all license fields
     * 2. Hash with SHA256
     * 3. Verify signature against public key
     */

    info->status = LICENSE_STATUS_VALID;
    CARI_LOG_INFO("License validated: %s (%s)", info->license_id,
             license_type_str(info->type));
    return LICENSE_STATUS_VALID;
}

bool license_has_feature(const license_info_t *info, license_feature_t feature) {
    if (!info || info->status != LICENSE_STATUS_VALID) {
        return false;
    }
    return (info->features & feature) != 0;
}

bool license_can_add(const license_info_t *info, const char *resource, int current) {
    int max = license_get_max(info, resource);
    if (max == 0) return true;  /* Unlimited */
    return current < max;
}

int license_get_max(const license_info_t *info, const char *resource) {
    if (!info) {
        /* Return free tier limits */
        if (strcmp(resource, "inputs") == 0) return LICENSE_FREE_MAX_INPUTS;
        if (strcmp(resource, "outputs") == 0) return LICENSE_FREE_MAX_OUTPUTS;
        if (strcmp(resource, "transcoders") == 0) return LICENSE_FREE_MAX_TRANSCODERS;
        if (strcmp(resource, "muxers") == 0) return LICENSE_FREE_MAX_MUXERS;
        if (strcmp(resource, "nodes") == 0) return LICENSE_FREE_MAX_NODES;
        return 0;
    }

    if (strcmp(resource, "inputs") == 0) return info->max_inputs;
    if (strcmp(resource, "outputs") == 0) return info->max_outputs;
    if (strcmp(resource, "transcoders") == 0) return info->max_transcoders;
    if (strcmp(resource, "muxers") == 0) return info->max_muxers;
    if (strcmp(resource, "nodes") == 0) return info->max_nodes;

    return 0;
}

const char* license_type_str(license_type_t type) {
    switch (type) {
        case LICENSE_TYPE_FREE:       return "Free";
        case LICENSE_TYPE_BASIC:      return "Basic";
        case LICENSE_TYPE_PRO:        return "Professional";
        case LICENSE_TYPE_ENTERPRISE: return "Enterprise";
        default:                      return "Unknown";
    }
}

const char* license_status_str(license_status_t status) {
    switch (status) {
        case LICENSE_STATUS_VALID:             return "Valid";
        case LICENSE_STATUS_INVALID:           return "Invalid";
        case LICENSE_STATUS_EXPIRED:           return "Expired";
        case LICENSE_STATUS_HARDWARE_MISMATCH: return "Hardware Mismatch";
        case LICENSE_STATUS_NOT_FOUND:         return "Not Found";
        case LICENSE_STATUS_TAMPERED:          return "Tampered";
        default:                               return "Unknown";
    }
}

int license_days_remaining(const license_info_t *info) {
    if (!info) return 0;
    if (info->is_perpetual) return -1;
    if (info->expiry_date == 0) return 0;

    time_t now = time(NULL);
    if (now >= info->expiry_date) return 0;

    return (int)((info->expiry_date - now) / 86400);
}

void license_init_free(license_info_t *info) {
    if (!info) return;

    memset(info, 0, sizeof(license_info_t));

    strncpy(info->license_id, "FREE", sizeof(info->license_id));
    strncpy(info->customer_name, "Unlicensed", sizeof(info->customer_name));

    info->type = LICENSE_TYPE_FREE;
    info->status = LICENSE_STATUS_VALID;
    info->is_perpetual = true;

    info->max_inputs = LICENSE_FREE_MAX_INPUTS;
    info->max_outputs = LICENSE_FREE_MAX_OUTPUTS;
    info->max_transcoders = LICENSE_FREE_MAX_TRANSCODERS;
    info->max_muxers = LICENSE_FREE_MAX_MUXERS;
    info->max_nodes = LICENSE_FREE_MAX_NODES;

    /* Free tier features */
    info->features = LICENSE_FEATURE_TRANSCODE;
}

int license_generate_request(const char *filename,
                              const char *customer_name,
                              const char *customer_email) {
    if (!filename || !customer_name || !customer_email) {
        return -1;
    }

    hardware_fingerprint_t fp;
    license_get_hardware_fingerprint(&fp);

    FILE *f = fopen(filename, "w");
    if (!f) {
        CARI_LOG_ERROR("Failed to create license request file");
        return -1;
    }

    fprintf(f, "# CariTranscoder License Request\n");
    fprintf(f, "# Send this file to licensing@example.com\n\n");
    fprintf(f, "[request]\n");
    fprintf(f, "customer = %s\n", customer_name);
    fprintf(f, "email = %s\n", customer_email);
    fprintf(f, "generated = %ld\n", time(NULL));
    fprintf(f, "\n[hardware]\n");
    fprintf(f, "fingerprint = %s\n", fp.fingerprint);
    fprintf(f, "machine_id = %s\n", fp.machine_id);
    fprintf(f, "mac_address = %s\n", fp.mac_address);

    fclose(f);
    CARI_LOG_INFO("License request generated: %s", filename);
    return 0;
}

void license_format_info(const license_info_t *info, char *buffer, size_t buflen) {
    if (!info || !buffer || buflen == 0) return;

    int days = license_days_remaining(info);
    char expiry_str[64];

    if (info->is_perpetual) {
        strncpy(expiry_str, "Perpetual", sizeof(expiry_str));
    } else if (days > 0) {
        snprintf(expiry_str, sizeof(expiry_str), "%d days remaining", days);
    } else {
        strncpy(expiry_str, "Expired", sizeof(expiry_str));
    }

    snprintf(buffer, buflen,
             "License: %s\n"
             "Type: %s\n"
             "Status: %s\n"
             "Customer: %s\n"
             "Expiry: %s\n"
             "Limits: %d inputs, %d outputs, %d transcoders, %d muxers\n"
             "HA Nodes: %d",
             info->license_id,
             license_type_str(info->type),
             license_status_str(info->status),
             info->customer_name,
             expiry_str,
             info->max_inputs ? info->max_inputs : -1,
             info->max_outputs ? info->max_outputs : -1,
             info->max_transcoders ? info->max_transcoders : -1,
             info->max_muxers ? info->max_muxers : -1,
             info->max_nodes);
}
