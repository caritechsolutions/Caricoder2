/*
 * CariTranscoder - INI Configuration Parser
 * Copyright (c) 2024 CariTech Solutions
 */

#ifndef CARI_CONFIG_H
#define CARI_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

/* Maximum sizes */
#define CONFIG_MAX_LINE         1024
#define CONFIG_MAX_SECTION      64
#define CONFIG_MAX_KEY          128
#define CONFIG_MAX_VALUE        512
#define CONFIG_MAX_ENTRIES      256

/* Configuration entry */
typedef struct {
    char section[CONFIG_MAX_SECTION];
    char key[CONFIG_MAX_KEY];
    char value[CONFIG_MAX_VALUE];
} config_entry_t;

/* Configuration context */
typedef struct {
    config_entry_t entries[CONFIG_MAX_ENTRIES];
    int entry_count;
    char filename[256];
    bool modified;
} config_t;

/**
 * Initialize configuration context
 *
 * @param cfg Configuration context
 */
void config_init(config_t *cfg);

/**
 * Load configuration from file
 *
 * @param cfg Configuration context
 * @param filename Path to configuration file
 * @return 0 on success, -1 on error
 */
int config_load(config_t *cfg, const char *filename);

/**
 * Save configuration to file
 *
 * @param cfg Configuration context
 * @param filename Path to file (NULL to use original filename)
 * @return 0 on success, -1 on error
 */
int config_save(config_t *cfg, const char *filename);

/**
 * Free configuration resources
 *
 * @param cfg Configuration context
 */
void config_free(config_t *cfg);

/**
 * Get string value
 *
 * @param cfg Configuration context
 * @param section Section name
 * @param key Key name
 * @param default_value Default if not found
 * @return Value string or default
 */
const char* config_get_string(const config_t *cfg, const char *section,
                               const char *key, const char *default_value);

/**
 * Get integer value
 *
 * @param cfg Configuration context
 * @param section Section name
 * @param key Key name
 * @param default_value Default if not found
 * @return Integer value
 */
int config_get_int(const config_t *cfg, const char *section,
                   const char *key, int default_value);

/**
 * Get unsigned integer value
 *
 * @param cfg Configuration context
 * @param section Section name
 * @param key Key name
 * @param default_value Default if not found
 * @return Unsigned integer value
 */
uint32_t config_get_uint(const config_t *cfg, const char *section,
                          const char *key, uint32_t default_value);

/**
 * Get 64-bit unsigned integer value
 */
uint64_t config_get_uint64(const config_t *cfg, const char *section,
                            const char *key, uint64_t default_value);

/**
 * Get boolean value
 *
 * @param cfg Configuration context
 * @param section Section name
 * @param key Key name
 * @param default_value Default if not found
 * @return Boolean value
 */
bool config_get_bool(const config_t *cfg, const char *section,
                     const char *key, bool default_value);

/**
 * Get float value
 *
 * @param cfg Configuration context
 * @param section Section name
 * @param key Key name
 * @param default_value Default if not found
 * @return Float value
 */
double config_get_float(const config_t *cfg, const char *section,
                        const char *key, double default_value);

/**
 * Set string value
 *
 * @param cfg Configuration context
 * @param section Section name
 * @param key Key name
 * @param value Value to set
 * @return 0 on success, -1 on error
 */
int config_set_string(config_t *cfg, const char *section,
                      const char *key, const char *value);

/**
 * Set integer value
 */
int config_set_int(config_t *cfg, const char *section,
                   const char *key, int value);

/**
 * Set boolean value
 */
int config_set_bool(config_t *cfg, const char *section,
                    const char *key, bool value);

/**
 * Check if key exists
 *
 * @param cfg Configuration context
 * @param section Section name
 * @param key Key name
 * @return true if key exists
 */
bool config_has_key(const config_t *cfg, const char *section, const char *key);

/**
 * Delete a key
 *
 * @param cfg Configuration context
 * @param section Section name
 * @param key Key name
 * @return 0 on success, -1 if not found
 */
int config_delete_key(config_t *cfg, const char *section, const char *key);

/**
 * Get all keys in a section
 *
 * @param cfg Configuration context
 * @param section Section name
 * @param keys Output array of key names
 * @param max_keys Maximum number of keys
 * @return Number of keys found
 */
int config_get_section_keys(const config_t *cfg, const char *section,
                            char keys[][CONFIG_MAX_KEY], int max_keys);

/**
 * Check if section exists
 *
 * @param cfg Configuration context
 * @param section Section name
 * @return true if section exists
 */
bool config_has_section(const config_t *cfg, const char *section);

/**
 * Dump configuration to log (debug)
 *
 * @param cfg Configuration context
 */
void config_dump(const config_t *cfg);

#endif /* CARI_CONFIG_H */
