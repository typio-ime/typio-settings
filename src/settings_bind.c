/**
 * @file settings_bind.c
 * @brief Simplified schema-driven config helpers.
 */

#include "settings_bind.h"
#include "typio/abi/config.h"

bool settings_bind_get_bool(const TypioConfig *config, const char *key,
                            bool fallback) {
    return typio_config_get_bool(config, key, fallback);
}

int settings_bind_get_int(const TypioConfig *config, const char *key,
                          int fallback) {
    return typio_config_get_int(config, key, fallback);
}

float settings_bind_get_float(const TypioConfig *config, const char *key,
                              float fallback) {
    return typio_config_get_float(config, key, fallback);
}

const char *settings_bind_get_string(const TypioConfig *config, const char *key,
                                     const char *fallback) {
    return typio_config_get_string(config, key, fallback);
}

void settings_bind_set_bool(TypioConfig *config, const char *key, bool value) {
    typio_config_set_bool(config, key, value);
}

void settings_bind_set_int(TypioConfig *config, const char *key, int value) {
    typio_config_set_int(config, key, value);
}

void settings_bind_set_float(TypioConfig *config, const char *key,
                             float value) {
    typio_config_set_float(config, key, value);
}

void settings_bind_set_string(TypioConfig *config, const char *key,
                              const char *value) {
    typio_config_set_string(config, key, value ? value : "");
}

const TypioConfigField *settings_bind_find_field(const char *key) {
    return typio_config_schema_find(key);
}
