#include "settings_internal.h"
#include "platform.h"
#include "typio/abi/config.h"
#include "typio/abi/string.h"
#include "typio/schema/config_schema.h"

#include <string.h>

/* These two helpers used to come from libtypio public headers
 * (`typio/abi/engine_label.h`, `typio/runtime/rime_schema_list.h`); both
 * were removed from libtypio's public API before this port. We provide
 * minimal local stubs so the settings panel builds.
 *
 * `typio_rime_schema_list_load` always returns `false`, which makes
 * `settings_refresh_rime_schema_options` populate the dropdown with only
 * "Unselected" plus the currently configured schema (if any). Browsing the
 * full schema list requires reintroducing a schema-list API in libtypio
 * — see the README "Note" for the planned generic engine property/command
 * mechanism that will replace this stub. */
static inline const char *typio_engine_label_fallback(const char *name) {
    return name && *name ? name : "Engine";
}

typedef struct TypioRimeSchemaList {
    char **schema_ids;
    size_t count;
} TypioRimeSchemaList;

static inline bool typio_rime_schema_list_load(
    [[maybe_unused]] const TypioConfig *rime_config,
    [[maybe_unused]] const char *default_data_dir,
    TypioRimeSchemaList *out) {
    if (out) {
        out->schema_ids = NULL;
        out->count = 0;
    }
    return false; /* not implemented — UI degrades to "Unselected" + configured value */
}

static inline void typio_rime_schema_list_clear(TypioRimeSchemaList *list) {
    if (!list) return;
    for (size_t i = 0; i < list->count; i++) g_free(list->schema_ids[i]);
    g_free(list->schema_ids);
    list->schema_ids = NULL;
    list->count = 0;
}

/* ================================================================
 * Forward declarations
 * ================================================================ */

static void settings_set_config_text(TypioSettings *settings, const char *text);
static void settings_update_engine_config_panel(TypioSettings *settings,
                                               const char *engine_name);
static void settings_materialize_current_engine_order(TypioSettings *settings);

/* ================================================================
 * String-array helpers
 * ================================================================ */

static guint settings_string_array_count(GPtrArray *values) {
    return values ? values->len : 0;
}

static const char *settings_string_array_get(GPtrArray *values, guint index) {
    if (!values || index >= values->len) {
        return NULL;
    }
    return g_ptr_array_index(values, index);
}

static gboolean settings_string_array_contains(GPtrArray *values,
                                               const char *value) {
    if (!values || !value) {
        return FALSE;
    }
    for (guint i = 0; i < values->len; ++i) {
        const char *item = g_ptr_array_index(values, i);
        if (item && g_strcmp0(item, value) == 0) {
            return TRUE;
        }
    }
    return FALSE;
}

static void settings_clear_string_array(GPtrArray *values) {
    if (!values) {
        return;
    }
    g_ptr_array_set_size(values, 0);
}

static guint settings_find_index_in_options(const char *const *options,
                                            const char *value) {
    if (!options || !value) {
        return G_MAXUINT;
    }
    for (guint i = 0; options[i]; i++) {
        if (strcmp(options[i], value) == 0) {
            return i;
        }
    }
    return G_MAXUINT;
}

/* ================================================================
 * Config text helpers
 * ================================================================ */

static char *settings_dup_buffer_text(TypioSettings *settings) {
    if (!settings) {
        return NULL;
    }
    return g_strdup(settings->config_text ? settings->config_text : "");
}

static char *settings_dup_config_string_from_text(const char *content,
                                                 const char *config_key) {
    TypioConfig *config = NULL;
    const char *value = NULL;
    char *result = NULL;

    if (!content || !config_key) {
        return NULL;
    }

    config = typio_config_load_string(content);
    if (!config) {
        return NULL;
    }

    value = typio_config_get_string(config, config_key, NULL);
    if (value && *value) {
        result = g_strdup(value);
    }

    typio_config_free(config);
    return result;
}

static char *settings_dup_staged_config_string(TypioSettings *settings,
                                              const char *config_key,
                                              const char *fallback_text) {
    char *content = NULL;
    char *result = NULL;

    if (settings && settings->config_text) {
        content = settings_dup_buffer_text(settings);
    } else if (fallback_text) {
        content = g_strdup(fallback_text);
    }

    if (!content) {
        return NULL;
    }

    result = settings_dup_config_string_from_text(content, config_key);
    g_free(content);
    return result;
}

static char *settings_dup_runtime_string_for_config_key(TypioSettings *settings,
                                                       const char *config_key) {
    if (!settings || !settings->client || !config_key) {
        return NULL;
    }
    const TipSnapshot *snap = tip_client_snapshot(settings->client);
    if (!snap || !snap->config_text) {
        return NULL;
    }
    return settings_dup_config_string_from_text(snap->config_text, config_key);
}

/* ================================================================
 * State-binding helpers
 * ================================================================ */

static guint settings_string_array_index(gpointer user_data, const char *value) {
    GPtrArray *values = user_data;
    if (!values || !value) {
        return G_MAXUINT;
    }
    for (guint i = 0; i < values->len; ++i) {
        const char *item = g_ptr_array_index(values, i);
        if (item && g_strcmp0(item, value) == 0) {
            return i;
        }
    }
    return G_MAXUINT;
}

static const char *settings_string_array_value(gpointer user_data, guint index) {
    GPtrArray *values = user_data;
    if (!values || index >= values->len) {
        return NULL;
    }
    return g_ptr_array_index(values, index);
}

static guint settings_rime_schema_index(gpointer user_data, const char *value) {
    return settings_string_array_index(user_data, value ? value : "");
}

static const char *settings_rime_schema_value(gpointer user_data, guint index) {
    const char *value = settings_string_array_value(user_data, index);
    if (!value || !*value) {
        return NULL;
    }
    return value;
}

static char *settings_dup_state_binding_value(TypioSettings *settings,
                                             const SettingsStateBinding *binding,
                                             const char *fallback_text) {
    if (!binding || !binding->config_key) {
        return NULL;
    }

    switch (binding->source) {
    case CONTROL_STATE_VALUE_FROM_CONFIG:
        return settings_dup_staged_config_string(settings, binding->config_key,
                                                 fallback_text);
    case CONTROL_STATE_VALUE_FROM_RUNTIME:
        return settings_dup_runtime_string_for_config_key(settings,
                                                          binding->config_key);
    case CONTROL_STATE_VALUE_RUNTIME_THEN_CONFIG:
        {
            char *result = settings_dup_runtime_string_for_config_key(
                settings, binding->config_key);
            if (result) {
                return result;
            }
            return settings_dup_staged_config_string(settings, binding->config_key,
                                                     fallback_text);
        }
    }

    return NULL;
}

static void settings_state_binding_select_value(const SettingsStateBinding *binding,
                                               const char *value) {
    if (!binding || !binding->selected_index || !binding->find_index) {
        return;
    }
    if (value && *value) {
        guint idx = binding->find_index(binding->user_data, value);
        *binding->selected_index = (idx != G_MAXUINT) ? (int)idx : -1;
    } else {
        *binding->selected_index = -1;
    }
}

static void settings_state_binding_load_from_config(const SettingsStateBinding *binding,
                                                   const TypioConfig *config) {
    const char *value;

    if (!binding || !binding->config_key || !config) {
        return;
    }

    value = typio_config_get_string(config, binding->config_key, NULL);
    settings_state_binding_select_value(binding, value);
}

static const char *settings_state_binding_get_selected_value(
    const SettingsStateBinding *binding) {
    if (!binding || !binding->selected_index || !binding->get_value) {
        return NULL;
    }
    if (*binding->selected_index < 0) {
        return NULL;
    }
    return binding->get_value(binding->user_data,
                              (guint)*binding->selected_index);
}

static void settings_state_binding_save_to_config(const SettingsStateBinding *binding,
                                                 TypioConfig *config) {
    const char *value;

    if (!binding || !binding->config_key || !config) {
        return;
    }

    value = settings_state_binding_get_selected_value(binding);
    if (value && *value) {
        typio_config_set_string(config, binding->config_key, value);
    }
}

static void settings_state_binding_refresh_options(const SettingsStateBinding *binding,
                                                   TypioSettings *settings,
                                                   const TypioConfig *config,
                                                   const char *configured_value) {
    if (!binding || !binding->refresh_options) {
        return;
    }
    binding->refresh_options(settings, config, configured_value);
}

static void settings_apply_state_binding_value(TypioSettings *settings,
                                              const SettingsStateBinding *binding,
                                              const char *fallback_text) {
    char *value;
    gboolean was_updating_ui;

    if (!settings || !binding) {
        return;
    }

    was_updating_ui = settings->updating_ui;
    settings->updating_ui = TRUE;
    value = settings_dup_state_binding_value(settings, binding, fallback_text);
    settings_state_binding_select_value(binding, value);
    settings->updating_ui = was_updating_ui;
    g_free(value);
}

/* ================================================================
 * UI sync helpers
 * ================================================================ */

static gboolean settings_is_ui_syncing(TypioSettings *settings) {
    return settings && settings->updating_ui;
}

static void settings_begin_ui_sync(TypioSettings *settings) {
    if (settings) {
        settings->updating_ui = TRUE;
    }
}

static void settings_end_ui_sync(TypioSettings *settings) {
    if (settings) {
        settings->updating_ui = FALSE;
    }
}

static const char *settings_resolve_config_text(const char *text) {
    return text ? text : "";
}

/* ================================================================
 * Status helpers
 * ================================================================ */

static void settings_set_inline_status(TypioSettings *settings,
                                      const char *text,
                                      gboolean visible) {
    if (!settings) {
        return;
    }
    g_free(settings->status_message);
    settings->status_message = g_strdup(text ? text : "");
    settings->status_visible = visible;
}

static gboolean settings_clear_status_timeout_cb(gpointer user_data) {
    TypioSettings *settings = user_data;

    if (!settings) {
        return G_SOURCE_REMOVE;
    }

    settings->status_clear_source_id = 0;
    settings_set_inline_status(settings, "", FALSE);
    return G_SOURCE_REMOVE;
}

static void settings_schedule_status_clear(TypioSettings *settings,
                                          guint delay_ms) {
    if (!settings) {
        return;
    }

    if (settings->status_clear_source_id != 0) {
        g_source_remove(settings->status_clear_source_id);
        settings->status_clear_source_id = 0;
    }

    if (delay_ms == 0) {
        settings_set_inline_status(settings, "", FALSE);
        return;
    }

    settings->status_clear_source_id =
        g_timeout_add(delay_ms, settings_clear_status_timeout_cb, settings);
}

/* ================================================================
 * Engine order helpers
 * ================================================================ */

[[maybe_unused]] static guint settings_engine_order_index(TypioSettings *settings,
                                         const char *engine_name) {
    if (!settings || !settings->engine_order || !engine_name) {
        return G_MAXUINT;
    }
    for (guint i = 0; i < settings->engine_order->len; ++i) {
        const char *item = g_ptr_array_index(settings->engine_order, i);
        if (item && g_strcmp0(item, engine_name) == 0) {
            return i;
        }
    }
    return G_MAXUINT;
}

static gboolean settings_has_custom_engine_order(TypioSettings *settings) {
    return settings && settings_string_array_count(settings->engine_order) > 0;
}

static guint settings_effective_engine_order_count(TypioSettings *settings) {
    guint custom_count;

    if (!settings) {
        return 0;
    }

    custom_count = settings_string_array_count(settings->engine_order);
    return custom_count > 0
               ? custom_count
               : settings_string_array_count(settings->engine_ids);
}

static const char *settings_effective_engine_order_name(TypioSettings *settings,
                                                        guint index) {
    guint custom_count;

    if (!settings) {
        return NULL;
    }

    custom_count = settings_string_array_count(settings->engine_order);
    if (custom_count > 0) {
        return settings_string_array_get(settings->engine_order, index);
    }

    return settings_string_array_get(settings->engine_ids, index);
}

static void settings_materialize_current_engine_order(TypioSettings *settings) {
    if (!settings || !settings->engine_order || !settings->engine_ids ||
        settings_has_custom_engine_order(settings)) {
        return;
    }

    for (guint i = 0; i < settings_string_array_count(settings->engine_ids); ++i) {
        char *copy = g_strdup(settings_string_array_get(settings->engine_ids, i));
        if (!copy || !*copy) {
            g_free(copy);
            continue;
        }
        g_ptr_array_add(settings->engine_order, copy);
    }
}

[[maybe_unused]] static char *settings_dup_engine_order_status(TypioSettings *settings,
                                              const char *engine_name) {
    GString *status;
    char *default_engine;
    gboolean is_active = FALSE;
    gboolean is_default = FALSE;
    gboolean is_available = FALSE;

    if (!engine_name || !*engine_name) {
        return g_strdup("");
    }

    status = g_string_new(NULL);
    default_engine = settings_dup_staged_config_string(settings, "default_engine",
                                                       NULL);
    is_default = default_engine && g_strcmp0(default_engine, engine_name) == 0;
    is_available = settings_string_array_contains(settings->engine_ids, engine_name);

    if (settings->engine_selected >= 0 && settings->engine_ids) {
        const char *active =
            settings_string_array_get(settings->engine_ids,
                                      (guint)settings->engine_selected);
        is_active = active && g_strcmp0(active, engine_name) == 0;
    }

    if (is_active) {
        g_string_append(status, "Active");
    }
    if (is_default) {
        if (status->len > 0) {
            g_string_append(status, " · ");
        }
        g_string_append(status, "Default");
    }
    if (!is_available) {
        if (status->len > 0) {
            g_string_append(status, " · ");
        }
        g_string_append(status, "Unavailable");
    }

    g_free(default_engine);
    return g_string_free(status, FALSE);
}

/* ================================================================
 * Config panel helpers
 * ================================================================ */

static void settings_update_engine_config_panel(TypioSettings *settings,
                                               const char *engine_name) {
    const char *visible_engine = NULL;

    if (!settings) {
        return;
    }

    visible_engine =
        (settings->engine_settings_engine && *settings->engine_settings_engine)
            ? settings->engine_settings_engine
            : engine_name;

    /* In the flux-ui port the UI layer reads engine_settings_engine and
     * engine_settings_open directly; no GTK stack / window to update. */
    (void)visible_engine;
}

static void settings_set_committed_config_text(TypioSettings *settings,
                                              const char *text) {
    if (!settings) {
        return;
    }

    g_free(settings->committed_config_text);
    settings->committed_config_text = g_strdup(settings_resolve_config_text(text));
}

static void settings_replace_staged_config_text(TypioSettings *settings,
                                               const char *text) {
    const char *resolved = settings_resolve_config_text(text);

    if (!settings) {
        return;
    }

    settings_begin_ui_sync(settings);
    g_free(settings->config_text);
    settings->config_text = g_strdup(resolved);
    settings_sync_form_from_buffer(settings);
    settings_end_ui_sync(settings);
}

static void settings_set_config_text(TypioSettings *settings, const char *text) {
    gboolean should_replace_stage;
    gboolean has_pending_local_changes;
    gboolean has_local_stage;
    char *staged_text;

    if (!settings || !text) {
        return;
    }

    staged_text = settings_dup_buffer_text(settings);
    has_local_stage = staged_text && *staged_text;
    has_pending_local_changes =
        has_local_stage && settings->committed_config_text &&
        g_strcmp0(staged_text, settings->committed_config_text) != 0;
    should_replace_stage = !has_pending_local_changes ||
                           (staged_text && g_strcmp0(staged_text, text) == 0);
    g_free(staged_text);

    settings->config_seeded = TRUE;
    settings_set_committed_config_text(settings, text);
    if (should_replace_stage) {
        settings_replace_staged_config_text(settings, text);
    }
}

/* ================================================================
 * Rime schema helpers
 * ================================================================ */

static void settings_append_rime_schema(TypioSettings *settings,
                                        const char *schema_id) {
    guint count;

    if (!settings || !settings->rime_schema_ids || !settings->rime_schema_labels ||
        !schema_id || !*schema_id) {
        return;
    }

    count = settings_string_array_count(settings->rime_schema_ids);
    for (guint i = 0; i < count; ++i) {
        const char *item = settings_string_array_get(settings->rime_schema_ids, i);
        if (item && g_strcmp0(item, schema_id) == 0) {
            return;
        }
    }

    g_ptr_array_add(settings->rime_schema_ids, g_strdup(schema_id));
    g_ptr_array_add(settings->rime_schema_labels, g_strdup(schema_id));
}

static void settings_clear_rime_schema_model(TypioSettings *settings) {
    if (!settings || !settings->rime_schema_ids || !settings->rime_schema_labels) {
        return;
    }

    settings_clear_string_array(settings->rime_schema_ids);
    settings_clear_string_array(settings->rime_schema_labels);
}

/* ================================================================
 * Voice model helpers
 * ================================================================ */

static void settings_select_voice_model_from_config(TypioSettings *settings,
                                                   TypioConfig *config) {
    const char *backend_name;
    char engine_model_key[256];
    const char *voice_model;

    if (!settings || !config) {
        return;
    }

    backend_name =
        settings_voice_backend_id(settings, (guint)settings->voice_backend_selected);
    if (!backend_name) {
        return;
    }
    g_snprintf(engine_model_key, sizeof(engine_model_key),
               "engines.%s.model", backend_name);
    voice_model = typio_config_get_string(config, engine_model_key, "");

    if (settings->voice_model_names) {
        for (guint i = 0; i < settings->voice_model_names->len; i++) {
            const char *name = g_ptr_array_index(settings->voice_model_names, i);
            if (name && g_strcmp0(name, voice_model) == 0) {
                settings->voice_model_selected = (int)i;
                return;
            }
        }
    }
    settings->voice_model_selected = -1;
}

/* ================================================================
 * TIP / proxy helpers
 * ================================================================ */

static bool settings_client_ready(TypioSettings *settings) {
    if (!settings || !settings->client) return false;
    const TipSnapshot *snap = tip_client_snapshot(settings->client);
    return snap && snap->connected;
}

static void settings_activate_engine(TypioSettings *settings,
                                     const char *engine_name) {
    if (!settings || !settings->client || !engine_name || !*engine_name) return;
    GError *error = NULL;
    if (!tip_engine_use(settings->client, engine_name, &error)) {
        g_warning("settings_activate_engine: engine=%s failed: %s",
                  engine_name,
                  error ? error->message : "Engine switch failed");
        settings_update_availability_label(settings,
            error ? error->message : "Engine switch failed", TRUE);
        g_clear_error(&error);
        return;
    }
    tip_client_refresh_snapshot(settings->client);
}

[[maybe_unused]] static void settings_apply_dirty_bindings(TypioSettings *settings) {
    if (!settings || !settings->client || !settings_client_ready(settings))
        return;
    const TipSnapshot *snap = tip_client_snapshot(settings->client);
    if (!snap || !snap->config_text) return;
    TypioConfig *baseline = typio_config_load_string(snap->config_text);
    if (!baseline) return;

    char *staged = settings_dup_buffer_text(settings);
    TypioConfig *staged_cfg = staged ? typio_config_load_string(staged) : NULL;
    g_free(staged);

    if (staged_cfg) {
        size_t n = typio_config_key_count(staged_cfg);
        for (size_t i = 0; i < n; i++) {
            char *key = typio_config_key_at(staged_cfg, i);
            if (!key) continue;
            const char *new_val = typio_config_get_string(staged_cfg, key, NULL);
            const char *old_val = typio_config_get_string(baseline, key, NULL);
            if (g_strcmp0(new_val, old_val) != 0 && new_val) {
                GError *error = NULL;
                if (!tip_config_set_string(settings->client, key, new_val, &error)) {
                    g_warning("config.set %s failed: %s", key,
                              error ? error->message : "?");
                    g_clear_error(&error);
                }
            }
            typio_free_string(key);
        }
        typio_config_free(staged_cfg);
    }
    typio_config_free(baseline);
    tip_client_refresh_snapshot(settings->client);
}

/* ================================================================
 * Engine / voice backend model setters
 * ================================================================ */

static void settings_set_language_model(TypioSettings *settings,
                                       const TipSnapshot *snap) {
    if (!settings) {
        return;
    }

    settings_begin_ui_sync(settings);
    settings_clear_string_array(settings->language_tags);
    settings->language_selected = -1;

    if (snap && snap->languages) {
        for (guint i = 0; i < snap->languages->len; i++) {
            const char *tag = g_ptr_array_index(snap->languages, i);
            if (!tag || !*tag)
                continue;
            if (snap->active_language &&
                g_strcmp0(tag, snap->active_language) == 0) {
                settings->language_selected =
                    (int)settings->language_tags->len;
            }
            g_ptr_array_add(settings->language_tags, g_strdup(tag));
        }
    }

    settings_end_ui_sync(settings);
}

static void settings_set_engine_model(TypioSettings *settings,
                                     const TipSnapshot *snap) {
    if (!settings) {
        return;
    }

    settings_begin_ui_sync(settings);
    settings_clear_string_array(settings->engine_ids);
    settings_clear_string_array(settings->engine_labels);

    if (snap && snap->engines) {
        for (guint i = 0; i < snap->engines->len; i++) {
            const TipEngineEntry *e = g_ptr_array_index(snap->engines, i);
            if (!e || !e->name || g_strcmp0(e->kind, "keyboard") != 0)
                continue;
            g_ptr_array_add(settings->engine_ids, g_strdup(e->name));
            const char *label = (e->display_name && *e->display_name)
                                    ? e->display_name
                                    : typio_engine_label_fallback(e->name);
            g_ptr_array_add(settings->engine_labels, g_strdup(label));
        }
    } else {
        settings->engine_selected = -1;
    }

    settings_end_ui_sync(settings);
    settings_refresh_engine_order_editor(settings);
}

static void settings_set_voice_backend_model(TypioSettings *settings,
                                            const TipSnapshot *snap) {
    if (!settings) {
        return;
    }

    settings_begin_ui_sync(settings);
    settings_clear_string_array(settings->voice_backend_ids);

    if (snap && snap->engines) {
        for (guint i = 0; i < snap->engines->len; i++) {
            const TipEngineEntry *e = g_ptr_array_index(snap->engines, i);
            if (!e || !e->name) continue;
            if (g_strcmp0(e->kind, "voice") == 0 ||
                is_voice_backend_name(e->name)) {
                g_ptr_array_add(settings->voice_backend_ids, g_strdup(e->name));
            }
        }
    }
    settings_end_ui_sync(settings);
}

/* ================================================================
 * Public API
 * ================================================================ */

TypioSettings *settings_new(void) {
    TypioSettings *settings = g_new0(TypioSettings, 1);

    settings->engine_selected = -1;
    settings->voice_backend_selected = -1;
    settings->voice_model_selected = -1;
    settings->rime_schema_selected = -1;
    settings->engine_order_add_selected = -1;
    settings->popup_theme_selected = -1;
    settings->candidate_layout_selected = -1;

    settings->language_selected = -1;

    settings->language_tags = g_ptr_array_new_with_free_func(g_free);
    settings->engine_ids = g_ptr_array_new_with_free_func(g_free);
    settings->engine_labels = g_ptr_array_new_with_free_func(g_free);
    settings->engine_order = g_ptr_array_new_with_free_func(g_free);
    settings->engine_available_ids = g_ptr_array_new_with_free_func(g_free);
    settings->engine_available_labels = g_ptr_array_new_with_free_func(g_free);
    settings->voice_backend_ids = g_ptr_array_new_with_free_func(g_free);
    settings->voice_model_names = g_ptr_array_new_with_free_func(g_free);
    settings->rime_schema_ids = g_ptr_array_new_with_free_func(g_free);
    settings->rime_schema_labels = g_ptr_array_new_with_free_func(g_free);

    /* State bindings */
    settings->keyboard_engine_state.config_key = "default_engine";
    settings->keyboard_engine_state.selected_index = &settings->engine_selected;
    settings->keyboard_engine_state.user_data = settings->engine_ids;
    settings->keyboard_engine_state.find_index = settings_string_array_index;
    settings->keyboard_engine_state.get_value = settings_string_array_value;
    settings->keyboard_engine_state.source = CONTROL_STATE_VALUE_FROM_RUNTIME;

    settings->voice_backend_state.config_key = "default_voice_engine";
    settings->voice_backend_state.selected_index =
        &settings->voice_backend_selected;
    settings->voice_backend_state.user_data = settings->voice_backend_ids;
    settings->voice_backend_state.find_index = settings_string_array_index;
    settings->voice_backend_state.get_value = settings_string_array_value;
    settings->voice_backend_state.source = CONTROL_STATE_VALUE_RUNTIME_THEN_CONFIG;

    settings->rime_schema_state.config_key = "engines.rime.schema";
    settings->rime_schema_state.selected_index = &settings->rime_schema_selected;
    settings->rime_schema_state.user_data = settings->rime_schema_ids;
    settings->rime_schema_state.find_index = settings_rime_schema_index;
    settings->rime_schema_state.get_value = settings_rime_schema_value;
    settings->rime_schema_state.source = CONTROL_STATE_VALUE_FROM_RUNTIME;
    settings->rime_schema_state.refresh_options =
        settings_refresh_rime_schema_options;

    return settings;
}

void settings_free(TypioSettings *settings) {
    if (!settings) {
        return;
    }

    settings_models_cleanup(settings);

    if (settings->status_clear_source_id != 0) {
        g_source_remove(settings->status_clear_source_id);
    }

    g_free(settings->config_text);
    g_free(settings->committed_config_text);
    g_free(settings->engine_settings_engine);
    g_free(settings->shortcut_switch_language);
    g_free(settings->shortcut_emergency_exit);
    g_free(settings->shortcut_voice_ptt);
    g_free(settings->status_message);

    g_ptr_array_free(settings->language_tags, TRUE);
    g_ptr_array_free(settings->engine_ids, TRUE);
    g_ptr_array_free(settings->engine_labels, TRUE);
    g_ptr_array_free(settings->engine_order, TRUE);
    g_ptr_array_free(settings->engine_available_ids, TRUE);
    g_ptr_array_free(settings->engine_available_labels, TRUE);
    g_ptr_array_free(settings->voice_backend_ids, TRUE);
    g_ptr_array_free(settings->voice_model_names, TRUE);
    g_ptr_array_free(settings->rime_schema_ids, TRUE);
    g_ptr_array_free(settings->rime_schema_labels, TRUE);

    g_free(settings);
}

gboolean settings_has_pending_config_change(TypioSettings *settings) {
    char *content;
    gboolean pending = FALSE;

    if (!settings || !settings->config_seeded || !settings->committed_config_text) {
        return FALSE;
    }

    content = settings_dup_buffer_text(settings);
    if (!content) {
        return FALSE;
    }

    pending = g_strcmp0(content, settings->committed_config_text) != 0;
    g_free(content);
    return pending;
}

void settings_update_availability_label(TypioSettings *settings,
                                        const char *message,
                                        gboolean visible) {
    if (!settings) {
        return;
    }
    g_free(settings->status_message);
    settings->status_message = g_strdup(message ? message : "");
    settings->status_visible = visible;
}

void settings_queue_autosave(TypioSettings *settings,
                             SettingsAutosavePriority priority) {
    guint delay_ms;

    if (!settings) return;

    delay_ms = (priority == CONTROL_AUTOSAVE_FAST) ? 75 : 250;
    if (settings->submitting_config || !settings_client_ready(settings))
        delay_ms = 1000;

    settings->autosave_priority = priority;
    settings->autosave_deadline = typio_platform_monotonic_ms() + delay_ms;
    settings->autosave_pending = true;
}

void settings_stage_form_change(TypioSettings *settings,
                                SettingsAutosavePriority priority) {
    if (!settings || !settings->config_seeded) {
        return;
    }

    settings_sync_buffer_from_form(settings);
    settings_queue_autosave(settings, priority);
}

void settings_sync_form_from_buffer(TypioSettings *settings) {
    char *content;
    TypioConfig *config;
    char *configured_schema;
    const TypioConfigField *field;

    if (!settings) {
        return;
    }

    content = settings_dup_buffer_text(settings);
    if (!content) {
        return;
    }

    config = typio_config_load_string(content);
    g_free(content);
    if (!config) {
        return;
    }

    settings_begin_ui_sync(settings);

    /* Display page */
    field = settings_bind_find_field("display.popup_theme");
    if (field && field->ui_options) {
        const char *value =
            typio_config_get_string(config, "display.popup_theme",
                                    field->def.s ? field->def.s : "");
        guint idx = settings_find_index_in_options(field->ui_options, value);
        settings->popup_theme_selected = (idx != G_MAXUINT) ? (int)idx : -1;
    }

    field = settings_bind_find_field("display.candidate_layout");
    if (field && field->ui_options) {
        const char *value =
            typio_config_get_string(config, "display.candidate_layout",
                                    field->def.s ? field->def.s : "");
        guint idx = settings_find_index_in_options(field->ui_options, value);
        settings->candidate_layout_selected = (idx != G_MAXUINT) ? (int)idx : -1;
    }

    settings->font_size =
        typio_config_get_float(config, "display.font_size", 11.0f);
    settings->popup_mode_indicator =
        typio_config_get_bool(config, "display.popup_mode_indicator", true);

    /* Notifications */
    settings->notifications_enable =
        typio_config_get_bool(config, "notifications.enable", true);
    settings->notifications_startup =
        typio_config_get_bool(config, "notifications.startup_checks", true);
    settings->notifications_runtime =
        typio_config_get_bool(config, "notifications.runtime", true);
    settings->notifications_voice =
        typio_config_get_bool(config, "notifications.voice", true);
    settings->notifications_cooldown =
        typio_config_get_float(config, "notifications.cooldown_ms", 15000.0f);

    /* Engine order */
    settings_load_engine_order_from_config(settings, config);

    /* Rime schema */
    configured_schema = settings_dup_runtime_string_for_config_key(
        settings, settings->rime_schema_state.config_key);
    settings_state_binding_refresh_options(&settings->rime_schema_state,
                                           settings, config, configured_schema);
    settings_apply_state_binding_value(settings, &settings->rime_schema_state,
                                       NULL);

    /* Voice backend */
    const char *voice_backend =
        typio_config_get_string(config,
                                settings->voice_backend_state.config_key, NULL);
    g_debug("settings_sync_form_from_buffer: default_voice_engine=%s "
            "voice_backend_index=%u",
            voice_backend ? voice_backend : "(unset)",
            settings_voice_backend_index(settings, voice_backend));
    settings_state_binding_load_from_config(&settings->voice_backend_state,
                                            config);

    /* Voice model sections */
    voice_update_model_sections(settings);
    settings_refresh_voice_models_from_stage(settings);

    /* Shortcuts */
    g_free(settings->shortcut_switch_language);
    settings->shortcut_switch_language =
        g_strdup(typio_config_get_string(config, "shortcuts.switch_language",
                                         "Ctrl+Shift"));
    g_free(settings->shortcut_emergency_exit);
    settings->shortcut_emergency_exit =
        g_strdup(typio_config_get_string(config, "shortcuts.emergency_exit",
                                         "Ctrl+Shift+Escape"));
    g_free(settings->shortcut_voice_ptt);
    settings->shortcut_voice_ptt =
        g_strdup(typio_config_get_string(config, "shortcuts.voice_ptt",
                                         "Super+v"));

    settings_end_ui_sync(settings);
    g_free(configured_schema);
    typio_config_free(config);
}

void settings_sync_buffer_from_form(TypioSettings *settings) {
    TypioConfig *config;
    const char *voice_backend;
    gboolean has_voice_backend_selection;
    const TypioConfigField *field;

    if (!settings || settings_is_ui_syncing(settings)) {
        return;
    }

    config = settings->config_text ? typio_config_load_string(settings->config_text)
                                   : NULL;
    if (!config) {
        config = typio_config_new();
    }
    if (!config) {
        g_warning("settings_sync_buffer_from_form: failed to load or create "
                  "config");
        return;
    }

    /* Display page */
    field = settings_bind_find_field("display.popup_theme");
    if (field && field->ui_options && settings->popup_theme_selected >= 0) {
        typio_config_set_string(config, "display.popup_theme",
                                field->ui_options[settings->popup_theme_selected]);
    }

    field = settings_bind_find_field("display.candidate_layout");
    if (field && field->ui_options &&
        settings->candidate_layout_selected >= 0) {
        typio_config_set_string(
            config, "display.candidate_layout",
            field->ui_options[settings->candidate_layout_selected]);
    }

    typio_config_set_float(config, "display.font_size", settings->font_size);
    typio_config_set_bool(config, "display.popup_mode_indicator",
                          settings->popup_mode_indicator);

    /* Notifications */
    typio_config_set_bool(config, "notifications.enable",
                          settings->notifications_enable);
    typio_config_set_bool(config, "notifications.startup_checks",
                          settings->notifications_startup);
    typio_config_set_bool(config, "notifications.runtime",
                          settings->notifications_runtime);
    typio_config_set_bool(config, "notifications.voice",
                          settings->notifications_voice);
    typio_config_set_float(config, "notifications.cooldown_ms",
                           settings->notifications_cooldown);

    /* Engine order */
    if (settings->engine_order && settings->engine_order->len > 0) {
        const char **ordered_names =
            g_new0(const char *, settings->engine_order->len);
        for (guint i = 0; i < settings->engine_order->len; ++i) {
            ordered_names[i] = g_ptr_array_index(settings->engine_order, i);
        }
        typio_config_set_string_array(config, "engine_order", ordered_names,
                                      settings->engine_order->len);
        g_free(ordered_names);
    } else {
        typio_config_remove(config, "engine_order");
    }

    /* Voice backend & model */
    voice_backend = settings_voice_backend_id(
        settings, (guint)settings->voice_backend_selected);
    has_voice_backend_selection = voice_backend && *voice_backend;
    g_debug("settings_sync_buffer_from_form: voice_backend=%s (selected=%d)",
            voice_backend ? voice_backend : "(unset)",
            settings->voice_backend_selected);
    if (has_voice_backend_selection) {
        settings_state_binding_save_to_config(&settings->voice_backend_state,
                                              config);

        if (settings->voice_model_selected >= 0 && settings->voice_model_names) {
            const char *voice_model = g_ptr_array_index(
                settings->voice_model_names, (guint)settings->voice_model_selected);
            if (voice_model && *voice_model) {
                char engine_model_key[256];
                g_snprintf(engine_model_key, sizeof(engine_model_key),
                           "engines.%s.model", voice_backend);
                typio_config_set_string(config, engine_model_key, voice_model);
            }
        }
    }

    /* Shortcuts */
    if (settings->shortcut_switch_language &&
        *settings->shortcut_switch_language) {
        typio_config_set_string(config, "shortcuts.switch_language",
                                settings->shortcut_switch_language);
    }
    if (settings->shortcut_emergency_exit &&
        *settings->shortcut_emergency_exit) {
        typio_config_set_string(config, "shortcuts.emergency_exit",
                                settings->shortcut_emergency_exit);
    }
    if (settings->shortcut_voice_ptt && *settings->shortcut_voice_ptt) {
        typio_config_set_string(config, "shortcuts.voice_ptt",
                                settings->shortcut_voice_ptt);
    }

    char *rendered = typio_config_to_string(config);
    typio_config_free(config);
    if (!rendered) {
        return;
    }

    settings_begin_ui_sync(settings);
    g_free(settings->config_text);
    settings->config_text = rendered;
    settings_end_ui_sync(settings);
}

void settings_refresh_voice_models_from_stage(TypioSettings *settings) {
    char *content;
    TypioConfig *config;

    if (!settings) {
        return;
    }

    settings_refresh_voice_models(settings);

    content = settings_dup_buffer_text(settings);
    config = content ? typio_config_load_string(content) : NULL;
    g_free(content);
    if (!config) {
        g_warning("settings_refresh_voice_models_from_stage: failed to parse "
                  "staged config");
        return;
    }

    settings_select_voice_model_from_config(settings, config);
    typio_config_free(config);
}

void on_display_dropdown_changed(TypioSettings *settings, int selected) {
    if (!settings || settings_is_ui_syncing(settings)) {
        return;
    }
    (void)selected;
    settings_stage_form_change(settings, CONTROL_AUTOSAVE_FAST);
}

void on_display_spin_changed(TypioSettings *settings, float value) {
    if (!settings || settings_is_ui_syncing(settings)) {
        return;
    }
    (void)value;
    settings_stage_form_change(settings, CONTROL_AUTOSAVE_NORMAL);
}

void on_display_switch_changed(TypioSettings *settings, bool value) {
    if (!settings || settings_is_ui_syncing(settings)) {
        return;
    }
    (void)value;
    settings_stage_form_change(settings, CONTROL_AUTOSAVE_FAST);
}

void on_voice_backend_changed(TypioSettings *settings, int selected) {
    if (!settings || settings_is_ui_syncing(settings)) {
        return;
    }
    (void)selected;
    g_debug("on_voice_backend_changed: user changed dropdown to %d (%s)",
            selected,
            settings_voice_backend_id(settings, (guint)selected));
    voice_update_model_sections(settings);
    settings_refresh_voice_models_from_stage(settings);
    settings_stage_form_change(settings, CONTROL_AUTOSAVE_FAST);
}

void on_language_selected(TypioSettings *settings, int selected) {
    const char *tag;

    if (!settings || settings_is_ui_syncing(settings)) {
        return;
    }

    if (selected < 0) {
        return;
    }

    tag = settings_string_array_get(settings->language_tags, (guint)selected);
    if (!tag || !*tag || !settings->client) {
        return;
    }

    GError *error = NULL;
    if (!tip_language_use(settings->client, tag, &error)) {
        g_warning("on_language_selected: tag=%s failed: %s", tag,
                  error ? error->message : "Language switch failed");
        settings_update_availability_label(settings,
            error ? error->message : "Language switch failed", TRUE);
        g_clear_error(&error);
        return;
    }
    tip_client_refresh_snapshot(settings->client);
}

void on_engine_selected(TypioSettings *settings, int selected) {
    const char *engine_name;

    if (!settings || settings_is_ui_syncing(settings)) {
        return;
    }

    if (selected < 0) {
        return;
    }

    engine_name = settings_string_array_get(settings->engine_ids, (guint)selected);
    settings_activate_engine(settings, engine_name);
}

void on_engine_order_add_clicked(TypioSettings *settings) {
    const char *engine_name;
    int selected;

    if (!settings) {
        return;
    }

    selected = settings->engine_order_add_selected;
    if (selected < 0) {
        return;
    }

    engine_name =
        settings_string_array_get(settings->engine_available_ids, (guint)selected);
    if (!engine_name || !*engine_name ||
        settings_string_array_contains(settings->engine_order, engine_name)) {
        return;
    }

    settings_materialize_current_engine_order(settings);
    g_ptr_array_add(settings->engine_order, g_strdup(engine_name));
    settings_refresh_engine_order_editor(settings);
    settings_stage_form_change(settings, CONTROL_AUTOSAVE_FAST);
}

void on_engine_order_move_up_clicked(TypioSettings *settings, int index) {
    if (!settings || index < 1 || !settings->engine_order) {
        return;
    }
    if ((guint)index >= settings->engine_order->len) {
        return;
    }

    settings_materialize_current_engine_order(settings);
    char *moved =
        g_strdup(g_ptr_array_index(settings->engine_order, (guint)index));
    g_ptr_array_remove_index(settings->engine_order, (guint)index);
    g_ptr_array_insert(settings->engine_order, (guint)index - 1, moved);
    g_free(moved);
    settings_refresh_engine_order_editor(settings);
    settings_stage_form_change(settings, CONTROL_AUTOSAVE_FAST);
}

void on_engine_order_move_down_clicked(TypioSettings *settings, int index) {
    if (!settings || index < 0 || !settings->engine_order) {
        return;
    }
    if ((guint)index + 1 >= settings->engine_order->len) {
        return;
    }

    settings_materialize_current_engine_order(settings);
    char *moved =
        g_strdup(g_ptr_array_index(settings->engine_order, (guint)index));
    g_ptr_array_remove_index(settings->engine_order, (guint)index);
    g_ptr_array_insert(settings->engine_order, (guint)index + 1, moved);
    g_free(moved);
    settings_refresh_engine_order_editor(settings);
    settings_stage_form_change(settings, CONTROL_AUTOSAVE_FAST);
}

void on_engine_order_remove_clicked(TypioSettings *settings, int index) {
    if (!settings || index < 0 || !settings->engine_order) {
        return;
    }
    if ((guint)index >= settings->engine_order->len) {
        return;
    }

    settings_materialize_current_engine_order(settings);
    g_ptr_array_remove_index(settings->engine_order, (guint)index);
    settings_refresh_engine_order_editor(settings);
    settings_stage_form_change(settings, CONTROL_AUTOSAVE_FAST);
}

void on_engine_activate_clicked(TypioSettings *settings, int index) {
    const char *engine_name;

    if (!settings || index < 0) {
        return;
    }

    settings_materialize_current_engine_order(settings);
    if ((guint)index >= settings_effective_engine_order_count(settings)) {
        return;
    }

    engine_name = settings_effective_engine_order_name(settings, (guint)index);
    if (!engine_name || !*engine_name) {
        return;
    }

    settings_activate_engine(settings, engine_name);
}

void on_engine_settings_edit_clicked(TypioSettings *settings, int index) {
    const char *engine_name;

    if (!settings || index < 0) {
        return;
    }

    settings_materialize_current_engine_order(settings);
    if ((guint)index >= settings_effective_engine_order_count(settings)) {
        return;
    }

    engine_name = settings_effective_engine_order_name(settings, (guint)index);
    if (!engine_name || !*engine_name) {
        return;
    }

    g_free(settings->engine_settings_engine);
    settings->engine_settings_engine = g_strdup(engine_name);
    settings->engine_settings_open = true;
    settings_update_engine_config_panel(settings, engine_name);
}

void on_engine_settings_window_close_request(TypioSettings *settings) {
    if (!settings) {
        return;
    }

    g_clear_pointer(&settings->engine_settings_engine, g_free);
    settings->engine_settings_open = false;
    settings_update_engine_config_panel(settings, NULL);
}

void on_engine_order_reset_clicked(TypioSettings *settings) {
    if (!settings || !settings->engine_order) {
        return;
    }

    if (settings->engine_order->len == 0) {
        return;
    }

    settings_clear_string_array(settings->engine_order);
    settings_refresh_engine_order_editor(settings);
    settings_stage_form_change(settings, CONTROL_AUTOSAVE_FAST);
}

void on_rime_deploy_clicked(TypioSettings *settings) {
    if (!settings || !settings_client_ready(settings)) return;

    GError *error = NULL;
    bool ok = tip_engine_invoke(settings->client, "rime", "deploy", &error);

    if (!ok) {
        g_warning("rime deploy failed: %s", error ? error->message : "?");
        settings_set_inline_status(
            settings,
            error ? error->message
                  : "Unable to deploy Rime configuration.",
            TRUE);
        settings_schedule_status_clear(settings, 3000);
        settings_update_availability_label(
            settings,
            error ? error->message : "Failed to deploy Rime configuration",
            TRUE);
        g_clear_error(&error);
        return;
    }
    settings_update_availability_label(settings, "", FALSE);
    settings_set_inline_status(settings, "Rime configuration deployed.", TRUE);
    settings_schedule_status_clear(settings, 2000);
    tip_client_refresh_snapshot(settings->client);
}

void settings_refresh_from_proxy(TypioSettings *settings) {
    if (!settings) return;

    const TipSnapshot *snap =
        settings->client ? tip_client_snapshot(settings->client) : NULL;

    if (!snap || !snap->connected) {
        settings_update_availability_label(
            settings, "Typio daemon is not running.", TRUE);
        settings_clear_rime_schema_model(settings);
        settings_set_language_model(settings, NULL);
        settings_set_engine_model(settings, NULL);
        settings_set_voice_backend_model(settings, NULL);
        settings_apply_state_binding_value(settings,
                                           &settings->keyboard_engine_state,
                                           NULL);
        settings_apply_state_binding_value(settings,
                                           &settings->voice_backend_state, NULL);
        return;
    }

    settings_update_availability_label(settings, "", FALSE);

    settings_set_language_model(settings, snap);
    settings_set_engine_model(settings, snap);
    settings_set_voice_backend_model(settings, snap);

    TypioConfig *parsed_config =
        snap->config_text ? typio_config_load_string(snap->config_text) : NULL;
    char *configured_schema = settings_dup_runtime_string_for_config_key(
        settings, settings->rime_schema_state.config_key);
    settings_state_binding_refresh_options(&settings->rime_schema_state, settings,
                                           parsed_config, configured_schema);

    settings_apply_state_binding_value(settings,
                                       &settings->keyboard_engine_state, NULL);
    settings_apply_state_binding_value(settings, &settings->voice_backend_state,
                                       snap->config_text);
    settings_update_engine_config_panel(
        settings, snap->active_keyboard ? snap->active_keyboard : "");
    settings_set_config_text(settings, snap->config_text);

    g_free(configured_schema);
    if (parsed_config) typio_config_free(parsed_config);
}

void settings_load_engine_order_from_config(TypioSettings *settings,
                                            const TypioConfig *config) {
    size_t order_count;
    gboolean restrict_to_known_keyboards;

    if (!settings) {
        return;
    }

    settings_clear_string_array(settings->engine_order);
    restrict_to_known_keyboards =
        settings_string_array_count(settings->engine_ids) > 0;
    order_count = config ? typio_config_get_array_size(config, "engine_order")
                         : 0;
    for (size_t i = 0; i < order_count; ++i) {
        const char *engine_name =
            typio_config_get_array_string(config, "engine_order", i);
        if (restrict_to_known_keyboards &&
            !settings_string_array_contains(settings->engine_ids, engine_name)) {
            continue;
        }
        g_ptr_array_add(settings->engine_order, g_strdup(engine_name));
    }

    settings_refresh_engine_order_editor(settings);
}

void settings_refresh_engine_order_editor(TypioSettings *settings) {
    guint order_count;

    if (!settings) {
        return;
    }

    settings_clear_string_array(settings->engine_available_ids);
    settings_clear_string_array(settings->engine_available_labels);

    order_count = settings_effective_engine_order_count(settings);
    if (order_count == 0) {
        /* UI layer shows empty state when no engines are available. */
    }

    for (guint i = 0; i < settings_string_array_count(settings->engine_ids); ++i) {
        const char *engine_name = settings_string_array_get(settings->engine_ids, i);
        if (!engine_name || !*engine_name ||
            settings_string_array_contains(settings->engine_order, engine_name)) {
            continue;
        }
        g_ptr_array_add(settings->engine_available_ids, g_strdup(engine_name));
        const char *label = settings_string_array_get(settings->engine_labels, i);
        g_ptr_array_add(settings->engine_available_labels,
                        g_strdup(label ? label : typio_engine_label_fallback(engine_name)));
    }

    settings->engine_order_has_custom = settings_has_custom_engine_order(settings);
    settings->engine_order_add_selected =
        settings->engine_available_ids->len > 0 ? 0 : -1;
}

void settings_refresh_rime_schema_options(TypioSettings *settings,
                                          const TypioConfig *config,
                                          const char *configured_value) {
    gboolean was_updating_ui;
    TypioConfig *rime_config = NULL;
    TypioRimeSchemaList list;
    const char *default_data_dir = NULL;
    char *data_dir_buf = NULL;

    if (!settings || !settings->rime_schema_ids || !settings->rime_schema_labels) {
        return;
    }

    was_updating_ui = settings_is_ui_syncing(settings);
    settings_begin_ui_sync(settings);
    settings_clear_rime_schema_model(settings);
    g_ptr_array_add(settings->rime_schema_ids, g_strdup(""));
    g_ptr_array_add(settings->rime_schema_labels, g_strdup("Unselected"));
    memset(&list, 0, sizeof(list));

    if (config) {
        rime_config = typio_config_get_section(config, "engines.rime");
        default_data_dir = typio_config_get_string(
            config, "engines.rime.user_data_dir", NULL);
    }

    if (!default_data_dir) {
        const char *data_home = g_get_user_data_dir();
        if (data_home) {
            data_dir_buf = g_build_filename(data_home, "typio", "rime", NULL);
            default_data_dir = data_dir_buf;
        }
    }

    if (typio_rime_schema_list_load(rime_config, default_data_dir, &list)) {
        for (size_t i = 0; i < list.count; ++i) {
            const char *id = list.schema_ids[i];
            g_debug("settings_refresh_rime_schema_options: schema=%s",
                    id ? id : "(null)");
            settings_append_rime_schema(settings, id);
        }
        typio_rime_schema_list_clear(&list);
    }

    if (configured_value && *configured_value) {
        settings_append_rime_schema(settings, configured_value);
    }

    g_debug("settings_refresh_rime_schema_options: configured_value=%s count=%u",
            configured_value ? configured_value : "(null)",
            settings->rime_schema_ids->len);
    settings->updating_ui = was_updating_ui;

    if (rime_config) {
        typio_config_free(rime_config);
    }
    g_free(data_dir_buf);
}

/* ================================================================
 * Test helpers
 * ================================================================ */

#ifdef TYPIO_SETTINGS_TEST
void settings_test_set_config_text(TypioSettings *settings,
                                   GVariant *config_text) {
    if (!settings || !config_text) {
        return;
    }
    gsize len;
    const char *text = g_variant_get_string(config_text, &len);
    settings_set_config_text(settings, text);
}

void settings_test_set_engine_model(TypioSettings *settings,
                                    GVariant *engines,
                                    GVariant *display_names) {
    if (!settings) {
        return;
    }

    settings_clear_string_array(settings->engine_ids);
    settings_clear_string_array(settings->engine_labels);

    if (engines) {
        gsize n_names;
        const char **names = g_variant_get_strv(engines, &n_names);
        gsize n_labels = 0;
        const char **labels = NULL;
        if (display_names) {
            labels = g_variant_get_strv(display_names, &n_labels);
        }
        for (gsize i = 0; i < n_names; i++) {
            g_ptr_array_add(settings->engine_ids, g_strdup(names[i]));
            const char *label = (labels && i < n_labels && labels[i] &&
                                 *labels[i])
                                    ? labels[i]
                                    : typio_engine_label_fallback(names[i]);
            g_ptr_array_add(settings->engine_labels, g_strdup(label));
        }
        g_free(names);
        g_free(labels);
    }

    settings_refresh_engine_order_editor(settings);
}

void settings_test_set_voice_backend_model(TypioSettings *settings,
                                           GVariant *engines) {
    if (!settings) {
        return;
    }

    settings_clear_string_array(settings->voice_backend_ids);

    if (engines) {
        gsize n;
        const char **names = g_variant_get_strv(engines, &n);
        for (gsize i = 0; i < n; i++) {
            g_ptr_array_add(settings->voice_backend_ids, g_strdup(names[i]));
        }
        g_free(names);
    }
}
#endif
