/*
 * CariTranscoder - INI Configuration Parser Implementation
 * Copyright (c) 2024 CariTech Solutions
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#include "config.h"
#include "logging.h"

/* Trim whitespace from string */
static char* trim(char *str) {
    if (!str) return NULL;

    /* Trim leading */
    while (isspace((unsigned char)*str)) str++;

    if (*str == '\0') return str;

    /* Trim trailing */
    char *end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    end[1] = '\0';

    return str;
}

/* Find entry by section and key */
static config_entry_t* find_entry(const config_t *cfg, const char *section,
                                   const char *key) {
    for (int i = 0; i < cfg->entry_count; i++) {
        if (strcasecmp(cfg->entries[i].section, section) == 0 &&
            strcasecmp(cfg->entries[i].key, key) == 0) {
            return (config_entry_t*)&cfg->entries[i];
        }
    }
    return NULL;
}

void config_init(config_t *cfg) {
    if (!cfg) return;
    memset(cfg, 0, sizeof(config_t));
}

int config_load(config_t *cfg, const char *filename) {
    if (!cfg || !filename) return -1;

    FILE *fp = fopen(filename, "r");
    if (!fp) {
        LOG_ERROR("Failed to open config file '%s': %s", filename, strerror(errno));
        return -1;
    }

    config_init(cfg);
    strncpy(cfg->filename, filename, sizeof(cfg->filename) - 1);

    char line[CONFIG_MAX_LINE];
    char current_section[CONFIG_MAX_SECTION] = "";
    int line_num = 0;

    while (fgets(line, sizeof(line), fp)) {
        line_num++;
        char *p = trim(line);

        /* Skip empty lines and comments */
        if (*p == '\0' || *p == '#' || *p == ';') {
            continue;
        }

        /* Section header */
        if (*p == '[') {
            char *end = strchr(p, ']');
            if (!end) {
                LOG_WARNING("Config %s:%d: Invalid section header", filename, line_num);
                continue;
            }
            *end = '\0';
            strncpy(current_section, trim(p + 1), sizeof(current_section) - 1);
            continue;
        }

        /* Key = value */
        char *eq = strchr(p, '=');
        if (!eq) {
            LOG_WARNING("Config %s:%d: Invalid line (no '=')", filename, line_num);
            continue;
        }

        *eq = '\0';
        char *key = trim(p);
        char *value = trim(eq + 1);

        /* Remove quotes from value if present */
        size_t vlen = strlen(value);
        if (vlen >= 2 && ((value[0] == '"' && value[vlen-1] == '"') ||
                          (value[0] == '\'' && value[vlen-1] == '\''))) {
            value[vlen-1] = '\0';
            value++;
        }

        /* Add entry */
        if (cfg->entry_count >= CONFIG_MAX_ENTRIES) {
            LOG_WARNING("Config %s: Maximum entries reached", filename);
            break;
        }

        config_entry_t *entry = &cfg->entries[cfg->entry_count++];
        strncpy(entry->section, current_section, sizeof(entry->section) - 1);
        strncpy(entry->key, key, sizeof(entry->key) - 1);
        strncpy(entry->value, value, sizeof(entry->value) - 1);
    }

    fclose(fp);
    LOG_DEBUG("Loaded %d config entries from '%s'", cfg->entry_count, filename);
    return 0;
}

int config_save(config_t *cfg, const char *filename) {
    if (!cfg) return -1;

    const char *fname = filename ? filename : cfg->filename;
    if (!fname[0]) return -1;

    FILE *fp = fopen(fname, "w");
    if (!fp) {
        LOG_ERROR("Failed to open config file '%s' for writing: %s",
                  fname, strerror(errno));
        return -1;
    }

    fprintf(fp, "# CariTranscoder Configuration\n");
    fprintf(fp, "# Auto-generated - edit with care\n\n");

    char current_section[CONFIG_MAX_SECTION] = "";

    for (int i = 0; i < cfg->entry_count; i++) {
        config_entry_t *entry = &cfg->entries[i];

        /* Write section header if changed */
        if (strcmp(entry->section, current_section) != 0) {
            if (current_section[0]) {
                fprintf(fp, "\n");
            }
            fprintf(fp, "[%s]\n", entry->section);
            strncpy(current_section, entry->section, sizeof(current_section) - 1);
        }

        /* Write key = value */
        fprintf(fp, "%s = %s\n", entry->key, entry->value);
    }

    fclose(fp);
    cfg->modified = false;
    LOG_DEBUG("Saved %d config entries to '%s'", cfg->entry_count, fname);
    return 0;
}

void config_free(config_t *cfg) {
    if (!cfg) return;
    memset(cfg, 0, sizeof(config_t));
}

const char* config_get_string(const config_t *cfg, const char *section,
                               const char *key, const char *default_value) {
    if (!cfg || !section || !key) return default_value;

    config_entry_t *entry = find_entry(cfg, section, key);
    return entry ? entry->value : default_value;
}

int config_get_int(const config_t *cfg, const char *section,
                   const char *key, int default_value) {
    const char *str = config_get_string(cfg, section, key, NULL);
    if (!str) return default_value;
    return atoi(str);
}

uint32_t config_get_uint(const config_t *cfg, const char *section,
                          const char *key, uint32_t default_value) {
    const char *str = config_get_string(cfg, section, key, NULL);
    if (!str) return default_value;
    return (uint32_t)strtoul(str, NULL, 0);
}

uint64_t config_get_uint64(const config_t *cfg, const char *section,
                            const char *key, uint64_t default_value) {
    const char *str = config_get_string(cfg, section, key, NULL);
    if (!str) return default_value;
    return strtoull(str, NULL, 0);
}

bool config_get_bool(const config_t *cfg, const char *section,
                     const char *key, bool default_value) {
    const char *str = config_get_string(cfg, section, key, NULL);
    if (!str) return default_value;

    if (strcasecmp(str, "true") == 0 ||
        strcasecmp(str, "yes") == 0 ||
        strcasecmp(str, "on") == 0 ||
        strcasecmp(str, "1") == 0) {
        return true;
    }

    if (strcasecmp(str, "false") == 0 ||
        strcasecmp(str, "no") == 0 ||
        strcasecmp(str, "off") == 0 ||
        strcasecmp(str, "0") == 0) {
        return false;
    }

    return default_value;
}

double config_get_float(const config_t *cfg, const char *section,
                        const char *key, double default_value) {
    const char *str = config_get_string(cfg, section, key, NULL);
    if (!str) return default_value;
    return atof(str);
}

int config_set_string(config_t *cfg, const char *section,
                      const char *key, const char *value) {
    if (!cfg || !section || !key || !value) return -1;

    config_entry_t *entry = find_entry(cfg, section, key);
    if (entry) {
        strncpy(entry->value, value, sizeof(entry->value) - 1);
    } else {
        if (cfg->entry_count >= CONFIG_MAX_ENTRIES) {
            LOG_ERROR("Config: Maximum entries reached");
            return -1;
        }
        entry = &cfg->entries[cfg->entry_count++];
        strncpy(entry->section, section, sizeof(entry->section) - 1);
        strncpy(entry->key, key, sizeof(entry->key) - 1);
        strncpy(entry->value, value, sizeof(entry->value) - 1);
    }

    cfg->modified = true;
    return 0;
}

int config_set_int(config_t *cfg, const char *section,
                   const char *key, int value) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", value);
    return config_set_string(cfg, section, key, buf);
}

int config_set_bool(config_t *cfg, const char *section,
                    const char *key, bool value) {
    return config_set_string(cfg, section, key, value ? "true" : "false");
}

bool config_has_key(const config_t *cfg, const char *section, const char *key) {
    return find_entry(cfg, section, key) != NULL;
}

int config_delete_key(config_t *cfg, const char *section, const char *key) {
    if (!cfg || !section || !key) return -1;

    for (int i = 0; i < cfg->entry_count; i++) {
        if (strcasecmp(cfg->entries[i].section, section) == 0 &&
            strcasecmp(cfg->entries[i].key, key) == 0) {
            /* Shift remaining entries */
            memmove(&cfg->entries[i], &cfg->entries[i + 1],
                    (cfg->entry_count - i - 1) * sizeof(config_entry_t));
            cfg->entry_count--;
            cfg->modified = true;
            return 0;
        }
    }

    return -1;
}

int config_get_section_keys(const config_t *cfg, const char *section,
                            char keys[][CONFIG_MAX_KEY], int max_keys) {
    if (!cfg || !section || !keys) return 0;

    int count = 0;
    for (int i = 0; i < cfg->entry_count && count < max_keys; i++) {
        if (strcasecmp(cfg->entries[i].section, section) == 0) {
            strncpy(keys[count], cfg->entries[i].key, CONFIG_MAX_KEY - 1);
            keys[count][CONFIG_MAX_KEY - 1] = '\0';
            count++;
        }
    }

    return count;
}

bool config_has_section(const config_t *cfg, const char *section) {
    if (!cfg || !section) return false;

    for (int i = 0; i < cfg->entry_count; i++) {
        if (strcasecmp(cfg->entries[i].section, section) == 0) {
            return true;
        }
    }

    return false;
}

void config_dump(const config_t *cfg) {
    if (!cfg) return;

    LOG_DEBUG("Config dump (%d entries):", cfg->entry_count);
    for (int i = 0; i < cfg->entry_count; i++) {
        LOG_DEBUG("  [%s] %s = %s",
                  cfg->entries[i].section,
                  cfg->entries[i].key,
                  cfg->entries[i].value);
    }
}
