#ifndef TYPIO_SETTINGS_BIND_H
#define TYPIO_SETTINGS_BIND_H

#include "typio/schema/config_schema.h"

bool        settings_bind_get_bool  (const TypioConfig *config, const char *key, bool   fallback);
int         settings_bind_get_int   (const TypioConfig *config, const char *key, int    fallback);
float       settings_bind_get_float (const TypioConfig *config, const char *key, float  fallback);
const char *settings_bind_get_string(const TypioConfig *config, const char *key, const char *fallback);

void settings_bind_set_bool  (TypioConfig *config, const char *key, bool   value);
void settings_bind_set_int   (TypioConfig *config, const char *key, int    value);
void settings_bind_set_float (TypioConfig *config, const char *key, float  value);
void settings_bind_set_string(TypioConfig *config, const char *key, const char *value);

const TypioConfigField *settings_bind_find_field(const char *key);

#endif
