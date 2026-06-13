#ifndef TYPIO_SETTINGS_INTERNAL_H
#define TYPIO_SETTINGS_INTERNAL_H

#include <flux/ui.h>
#include <gio/gio.h>
#include <stdbool.h>

#include "settings_bind.h"
#include "tip_client.h"

enum {
    WHISPER_MODEL_COUNT = 5,
    SHERPA_MODEL_COUNT = 4,
    VOICE_BACKEND_COUNT = 2,
};

struct TypioSettings;

typedef enum SettingsAutosavePriority {
    CONTROL_AUTOSAVE_NORMAL = 0,
    CONTROL_AUTOSAVE_FAST = 1,
} SettingsAutosavePriority;

typedef struct ModelInfo {
    const char *name;
    const char *display_name;
    const char *size_label;
    gint64 expected_size;
    const char *url;
    const char *filename;
    const char *extract_dir;
} ModelInfo;

typedef struct ModelRow {
    struct TypioSettings *settings;
    const ModelInfo *info;
    char *status_text;
    bool downloading;
    float download_progress;
    char *progress_text;
    GSubprocess *download_proc;
    guint progress_timer;
    char *installed_path;
    char *tmp_path;
    char *base_dir;
} ModelRow;

typedef guint (*SettingsStateIndexFunc)(gpointer user_data, const char *value);
typedef const char *(*SettingsStateValueFunc)(gpointer user_data, guint index);
typedef void (*SettingsStateOptionsRefreshFunc)(struct TypioSettings *settings,
                                                const TypioConfig *config,
                                                const char *configured_value);

typedef enum SettingsStateValueSource {
    CONTROL_STATE_VALUE_FROM_CONFIG = 0,
    CONTROL_STATE_VALUE_FROM_RUNTIME = 1,
    CONTROL_STATE_VALUE_RUNTIME_THEN_CONFIG = 2,
} SettingsStateValueSource;

typedef struct SettingsStateBinding {
    const char *config_key;
    int *selected_index;
    gpointer user_data;
    SettingsStateIndexFunc find_index;
    SettingsStateValueFunc get_value;
    SettingsStateValueSource source;
    SettingsStateOptionsRefreshFunc refresh_options;
} SettingsStateBinding;

typedef struct TypioSettings {
    /* Config staging */
    char *config_text;
    char *committed_config_text;
    bool config_seeded;
    bool updating_ui;

    /* Page tabs */
    int active_tab;

    /* --- Display page --- */
    int popup_theme_selected;
    int candidate_layout_selected;
    float font_size;
    bool popup_mode_indicator;

    /* Notifications */
    bool notifications_enable;
    bool notifications_startup;
    bool notifications_runtime;
    bool notifications_voice;
    float notifications_cooldown;

    /* --- Engines page --- */
    /* Languages (the user-facing switch unit, typio-linux ADR-0031) */
    GPtrArray *language_tags;
    int language_selected;

    GPtrArray *engine_ids;
    GPtrArray *engine_labels;
    int engine_selected;

    /* Engine order */
    GPtrArray *engine_order;
    GPtrArray *engine_available_ids;
    GPtrArray *engine_available_labels;
    int engine_order_add_selected;
    bool engine_order_has_custom;

    /* Engine settings modal */
    bool engine_settings_open;
    char *engine_settings_engine;

    /* Rime */
    GPtrArray *rime_schema_ids;
    GPtrArray *rime_schema_labels;
    int rime_schema_selected;

    /* Voice */
    GPtrArray *voice_backend_ids;
    int voice_backend_selected;
    GPtrArray *voice_model_names;
    int voice_model_selected;

    /* --- Shortcuts page --- */
    char *shortcut_switch_language;
    char *shortcut_emergency_exit;
    char *shortcut_voice_ptt;
    bool recording_shortcut;
    int recording_shortcut_index;

    /* --- Status --- */
    char *status_message;
    bool status_visible;
    guint status_clear_source_id;

    /* --- TIP client --- */
    TipClient *client;
    guint64 last_reconnect_time;
    bool reconnect_scheduled;

    /* --- Autosave --- */
    bool autosave_pending;
    guint64 autosave_deadline;
    bool submitting_config;
    SettingsAutosavePriority autosave_priority;

    /* --- Models --- */
    ModelRow whisper_rows[WHISPER_MODEL_COUNT];
    char *whisper_dir;
    ModelRow sherpa_rows[SHERPA_MODEL_COUNT];
    char *sherpa_dir;

    /* State bindings */
    SettingsStateBinding keyboard_engine_state;
    SettingsStateBinding voice_backend_state;
    SettingsStateBinding rime_schema_state;
} TypioSettings;

/* Lifecycle */
TypioSettings *settings_new(void);
void settings_free(TypioSettings *settings);

/* Models */
const char *settings_voice_backend_id(TypioSettings *settings, guint idx);
guint settings_voice_backend_index(TypioSettings *settings, const char *id);
gboolean is_voice_backend_name(const char *name);
void voice_update_model_sections(TypioSettings *settings);
void settings_refresh_voice_models(TypioSettings *settings);
void settings_build_whisper_model_section(fx_ui *ui, TypioSettings *settings);
void settings_build_sherpa_model_section(fx_ui *ui, TypioSettings *settings);
void settings_models_cleanup(TypioSettings *settings);

/* Config sync */
void settings_sync_form_from_buffer(TypioSettings *settings);
void settings_sync_buffer_from_form(TypioSettings *settings);
void settings_stage_form_change(TypioSettings *settings,
                                SettingsAutosavePriority priority);
void settings_refresh_voice_models_from_stage(TypioSettings *settings);

/* Event handlers */
void on_display_dropdown_changed(TypioSettings *settings, int selected);
void on_display_spin_changed(TypioSettings *settings, float value);
void on_display_switch_changed(TypioSettings *settings, bool value);
void on_voice_backend_changed(TypioSettings *settings, int selected);
void on_language_selected(TypioSettings *settings, int selected);
void on_engine_selected(TypioSettings *settings, int selected);
void on_engine_order_add_clicked(TypioSettings *settings);
void on_engine_order_move_up_clicked(TypioSettings *settings, int index);
void on_engine_order_move_down_clicked(TypioSettings *settings, int index);
void on_engine_order_remove_clicked(TypioSettings *settings, int index);
void on_engine_activate_clicked(TypioSettings *settings, int index);
void on_engine_settings_edit_clicked(TypioSettings *settings, int index);
void on_engine_settings_window_close_request(TypioSettings *settings);
void on_engine_order_reset_clicked(TypioSettings *settings);
void on_rime_deploy_clicked(TypioSettings *settings);

/* TIP / proxy */
void settings_clear_proxy(TypioSettings *settings);
void settings_refresh_from_proxy(TypioSettings *settings);
gboolean settings_has_pending_config_change(TypioSettings *settings);
void settings_queue_autosave(TypioSettings *settings,
                             SettingsAutosavePriority priority);
void settings_update_availability_label(TypioSettings *settings,
                                        const char *message,
                                        gboolean visible);
void settings_load_engine_order_from_config(TypioSettings *settings,
                                            const TypioConfig *config);
void settings_refresh_engine_order_editor(TypioSettings *settings);
void settings_refresh_rime_schema_options(TypioSettings *settings,
                                          const TypioConfig *config,
                                          const char *configured_value);

/* UI building */
void settings_build_ui(fx_ui *ui, TypioSettings *settings);

/* Debug naming helpers */
char *settings_build_debug_name(const char *prefix, const char *token);

/* Test helpers */
#ifdef TYPIO_SETTINGS_TEST
void settings_test_set_config_text(TypioSettings *settings,
                                   GVariant *config_text);
void settings_test_set_engine_model(TypioSettings *settings,
                                    GVariant *engines,
                                    GVariant *display_names);
void settings_test_set_voice_backend_model(TypioSettings *settings,
                                           GVariant *engines);
#endif

#endif
