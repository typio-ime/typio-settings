#include "settings_internal.h"
#include "settings_bind.h"

#include "typio/typio.h"

#include <string.h>

/* -------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* -------------------------------------------------------------------------- */

static guint ptr_array_len(GPtrArray *arr)
{
    return arr ? arr->len : 0;
}

static const char *ptr_array_get(GPtrArray *arr, guint idx)
{
    return (arr && idx < arr->len) ? (const char *)g_ptr_array_index(arr, idx) : NULL;
}

static const char *engine_label_fallback(const char *name)
{
    return (name && *name) ? name : "Engine";
}

static bool engine_order_is_active(TypioSettings *settings, const char *engine_name)
{
    return settings->engine_selected >= 0 &&
           settings->engine_selected < (int)ptr_array_len(settings->engine_ids) &&
           g_strcmp0(ptr_array_get(settings->engine_ids, settings->engine_selected), engine_name) == 0;
}

static bool engine_order_is_available(TypioSettings *settings, const char *engine_name)
{
    for (guint i = 0; i < ptr_array_len(settings->engine_ids); i++) {
        if (g_strcmp0(ptr_array_get(settings->engine_ids, i), engine_name) == 0)
            return true;
    }
    return false;
}

static char *engine_order_status(TypioSettings *settings, const char *engine_name)
{
    GString *s = g_string_new(NULL);
    bool is_active = engine_order_is_active(settings, engine_name);
    bool is_available = engine_order_is_available(settings, engine_name);
    char *default_engine = NULL;

    if (settings->config_text) {
        TypioConfig *config = typio_config_load_string(settings->config_text);
        if (config) {
            default_engine = g_strdup(typio_config_get_string(config, "default_engine", NULL));
            typio_config_free(config);
        }
    }
    bool is_default = default_engine && g_strcmp0(default_engine, engine_name) == 0;

    if (is_active)
        g_string_append(s, "Active");
    if (is_default) {
        if (s->len > 0)
            g_string_append(s, " · ");
        g_string_append(s, "Default");
    }
    if (!is_available) {
        if (s->len > 0)
            g_string_append(s, " · ");
        g_string_append(s, "Unavailable");
    }

    g_free(default_engine);
    return g_string_free(s, FALSE);
}

static guint find_string_in_options(const char *const *options, const char *value)
{
    if (!options || !value)
        return 0;
    for (guint i = 0; options[i]; i++) {
        if (strcmp(options[i], value) == 0)
            return i;
    }
    return 0;
}

static void pref_label_column(fx_ui *ui, const char *title, const char *description)
{
    fx_column_ex(ui, (fx_layout_opts){ .cross = FX_START });
    fx_label_ex(ui, title, 16);
    fx_label(ui, description);
    fx_end(ui);
}

/* -------------------------------------------------------------------------- */
/* Display page                                                               */
/* -------------------------------------------------------------------------- */

static void build_display_page(fx_ui *ui, TypioSettings *settings)
{
    const TypioConfigField *field;

    fx_scroll_begin(ui, "display-page");
    fx_column(ui);

    fx_heading(ui, "Appearance", 2);
    fx_label(ui, "Tune the popup layout and panel notifications without leaving the session.");

    /* Popup theme */
    field = settings_bind_find_field("display.popup_theme");
    if (field && field->ui_options) {
        fx_row(ui);
        pref_label_column(ui, "Popup theme",
            "Choose whether the candidate popup follows the desktop theme or a fixed appearance.");
        fx_flex(ui, 1.0f);
        if (fx_dropdown(ui, "", &settings->popup_theme_selected,
                        (const char **)field->ui_options, g_strv_length((gchar **)field->ui_options))) {
            on_display_dropdown_changed(settings, settings->popup_theme_selected);
        }
        fx_end(ui);
    }

    /* Candidate layout */
    field = settings_bind_find_field("display.candidate_layout");
    if (field && field->ui_options) {
        fx_row(ui);
        pref_label_column(ui, "Candidate layout",
            "Select horizontal or vertical candidate arrangement for popup rendering.");
        fx_flex(ui, 1.0f);
        if (fx_dropdown(ui, "", &settings->candidate_layout_selected,
                        (const char **)field->ui_options, g_strv_length((gchar **)field->ui_options))) {
            on_display_dropdown_changed(settings, settings->candidate_layout_selected);
        }
        fx_end(ui);
    }

    /* Font size */
    field = settings_bind_find_field("display.font_size");
    if (field) {
        fx_row(ui);
        pref_label_column(ui, "Font size",
            "Adjust the popup text size for candidate and preedit content.");
        fx_flex(ui, 1.0f);
        if (fx_slider(ui, "", &settings->font_size, field->ui_min, field->ui_max)) {
            on_display_spin_changed(settings, settings->font_size);
        }
        fx_end(ui);
    }

    /* Mode indicator */
    fx_row(ui);
    pref_label_column(ui, "Mode indicator",
        "Show or hide the engine mode label in the candidate popup.");
    fx_flex(ui, 1.0f);
    if (fx_checkbox(ui, "", &settings->popup_mode_indicator)) {
        on_display_switch_changed(settings, settings->popup_mode_indicator);
    }
    fx_end(ui);

    fx_separator(ui);

    fx_heading(ui, "Notifications", 2);
    fx_label(ui, "Keep runtime alerts useful without turning the panel into a dashboard.");

    /* Enable notifications */
    fx_row(ui);
    pref_label_column(ui, "Enable notifications",
        "Master switch for panel-managed notification behavior.");
    fx_flex(ui, 1.0f);
    if (fx_checkbox(ui, "", &settings->notifications_enable)) {
        on_display_switch_changed(settings, settings->notifications_enable);
    }
    fx_end(ui);

    /* Startup checks */
    fx_row(ui);
    pref_label_column(ui, "Startup checks",
        "Show startup warnings when Typio detects missing runtime prerequisites.");
    fx_flex(ui, 1.0f);
    if (fx_checkbox(ui, "", &settings->notifications_startup)) {
        on_display_switch_changed(settings, settings->notifications_startup);
    }
    fx_end(ui);

    /* Runtime alerts */
    fx_row(ui);
    pref_label_column(ui, "Runtime alerts",
        "Show alerts for service or backend issues during normal operation.");
    fx_flex(ui, 1.0f);
    if (fx_checkbox(ui, "", &settings->notifications_runtime)) {
        on_display_switch_changed(settings, settings->notifications_runtime);
    }
    fx_end(ui);

    /* Voice alerts */
    fx_row(ui);
    pref_label_column(ui, "Voice alerts",
        "Show voice-backend model and microphone related notifications.");
    fx_flex(ui, 1.0f);
    if (fx_checkbox(ui, "", &settings->notifications_voice)) {
        on_display_switch_changed(settings, settings->notifications_voice);
    }
    fx_end(ui);

    /* Cooldown */
    field = settings_bind_find_field("notifications.cooldown_ms");
    if (field) {
        fx_row(ui);
        pref_label_column(ui, "Cooldown (ms)",
            "Debounce repeated notifications so transient issues do not spam the desktop.");
        fx_flex(ui, 1.0f);
        if (fx_slider(ui, "", &settings->notifications_cooldown, field->ui_min, field->ui_max)) {
            on_display_spin_changed(settings, settings->notifications_cooldown);
        }
        fx_end(ui);
    }

    fx_end(ui); /* column */
    fx_scroll_end(ui);
}

/* -------------------------------------------------------------------------- */
/* Engine config sections (shown inside the modal area)                       */
/* -------------------------------------------------------------------------- */

static void build_basic_config(fx_ui *ui, TypioSettings *settings)
{
    const TypioConfigField *field;
    int printable_key_mode_selected = 0;
    bool compose = false;

    if (settings->config_text) {
        TypioConfig *config = typio_config_load_string(settings->config_text);
        if (config) {
            field = settings_bind_find_field("engines.basic.printable_key_mode");
            if (field && field->ui_options) {
                const char *val = settings_bind_get_string(config, "engines.basic.printable_key_mode", field->def.s);
                printable_key_mode_selected = find_string_in_options(field->ui_options, val);
            }
            field = settings_bind_find_field("engines.basic.compose");
            if (field) {
                compose = settings_bind_get_bool(config, "engines.basic.compose", field->def.b);
            }
            typio_config_free(config);
        }
    }

    field = settings_bind_find_field("engines.basic.printable_key_mode");
    if (field && field->ui_options) {
        fx_row(ui);
        pref_label_column(ui, "Printable keys",
            "Forward key events or let Typio commit text directly.");
        fx_flex(ui, 1.0f);
        if (fx_dropdown(ui, "", &printable_key_mode_selected,
                        (const char **)field->ui_options, g_strv_length((gchar **)field->ui_options))) {
            TypioConfig *config = typio_config_load_string(settings->config_text ? settings->config_text : "");
            if (!config)
                config = typio_config_new();
            settings_bind_set_string(config, "engines.basic.printable_key_mode",
                                     field->ui_options[printable_key_mode_selected]);
            char *rendered = typio_config_to_string(config);
            if (rendered) {
                g_free(settings->config_text);
                settings->config_text = rendered;
                settings_stage_form_change(settings, CONTROL_AUTOSAVE_FAST);
            }
            typio_config_free(config);
        }
        fx_end(ui);
    }

    field = settings_bind_find_field("engines.basic.compose");
    if (field) {
        fx_row(ui);
        pref_label_column(ui, "Compose sequences",
            "Enable compose key sequences (e.g. ' + a → á).");
        fx_flex(ui, 1.0f);
        if (fx_checkbox(ui, "", &compose)) {
            TypioConfig *config = typio_config_load_string(settings->config_text ? settings->config_text : "");
            if (!config)
                config = typio_config_new();
            settings_bind_set_bool(config, "engines.basic.compose", compose);
            char *rendered = typio_config_to_string(config);
            if (rendered) {
                g_free(settings->config_text);
                settings->config_text = rendered;
                settings_stage_form_change(settings, CONTROL_AUTOSAVE_FAST);
            }
            typio_config_free(config);
        }
        fx_end(ui);
    }
}

static void build_rime_config(fx_ui *ui, TypioSettings *settings)
{
    fx_heading(ui, "Rime configuration", 2);

    fx_row(ui);
    pref_label_column(ui, "Schema", "Choose the active Rime schema.");
    fx_flex(ui, 1.0f);
    if (fx_dropdown(ui, "", &settings->rime_schema_selected,
                    (const char **)settings->rime_schema_labels->pdata,
                    ptr_array_len(settings->rime_schema_labels))) {
        on_display_dropdown_changed(settings, settings->rime_schema_selected);
    }
    fx_end(ui);

    fx_row(ui);
    pref_label_column(ui, "Deploy configuration",
        "Rebuild Rime data after editing custom schema files.");
    fx_flex(ui, 1.0f);
    if (fx_button(ui, "Deploy")) {
        on_rime_deploy_clicked(settings);
    }
    fx_end(ui);
}

static void build_mozc_config(fx_ui *ui, TypioSettings *settings)
{
    (void)settings;
    fx_label(ui, "No configurable Mozc options are exposed here yet.");
}

/* -------------------------------------------------------------------------- */
/* Engines page                                                               */
/* -------------------------------------------------------------------------- */

static void build_engines_page(fx_ui *ui, TypioSettings *settings)
{
    const TypioConfigField *field;
    guint order_count;
    guint available_count;
    const char *backend_id;

    fx_scroll_begin(ui, "engines-page");
    fx_column(ui);

    fx_heading(ui, "Input engines", 2);
    fx_label(ui, "Typio has two parallel engine categories: keyboard for composition and candidates, and voice for speech recognition. Each category keeps its own single active engine.");

    /* --- Languages (the user-facing switch unit, ADR-0031) --- */
    if (ptr_array_len(settings->language_tags) > 0) {
        fx_heading(ui, "Language", 2);
        fx_label(ui, "The active language picks the keyboard and voice engines together. A language without a keyboard engine is layout-only: keys pass through with the system layout.");

        fx_row(ui);
        pref_label_column(ui, "Active language",
            "Cycle with the switch-language shortcut, or pick one here.");
        fx_flex(ui, 1.0f);
        if (fx_dropdown(ui, "", &settings->language_selected,
                        (const char **)settings->language_tags->pdata,
                        ptr_array_len(settings->language_tags))) {
            on_language_selected(settings, settings->language_selected);
        }
        fx_end(ui);
        fx_separator(ui);
    }

    /* --- Keyboard engines --- */
    fx_heading(ui, "Keyboard engines", 2);
    fx_label(ui, "Keyboard input is the primary engine category. Exactly one keyboard engine is active at a time, and it owns composition, candidates, and commit behavior.");

    /* Per-app preferences */
    field = settings_bind_find_field("keyboard.per_app_preferences");
    if (field) {
        bool per_app = false;
        if (settings->config_text) {
            TypioConfig *config = typio_config_load_string(settings->config_text);
            if (config) {
                per_app = settings_bind_get_bool(config, "keyboard.per_app_preferences", field->def.b);
                typio_config_free(config);
            }
        }
        fx_row(ui);
        pref_label_column(ui, "Per-app preferences",
            "Remember and restore the keyboard engine together with its mode for each application identity.");
        fx_flex(ui, 1.0f);
        if (fx_checkbox(ui, "", &per_app)) {
            TypioConfig *config = typio_config_load_string(settings->config_text ? settings->config_text : "");
            if (!config)
                config = typio_config_new();
            settings_bind_set_bool(config, "keyboard.per_app_preferences", per_app);
            char *rendered = typio_config_to_string(config);
            if (rendered) {
                g_free(settings->config_text);
                settings->config_text = rendered;
                settings_stage_form_change(settings, CONTROL_AUTOSAVE_FAST);
            }
            typio_config_free(config);
        }
        fx_end(ui);
    }

    /* Engine order */
    fx_label(ui, settings->engine_order_has_custom
        ? "Custom order is active. Use the buttons to reorder or remove engines from the custom list."
        : "Automatic order is active. This list is currently following the runtime engine discovery order.");

    order_count = ptr_array_len(settings->engine_order);
    if (order_count == 0) {
        fx_label(ui, "No keyboard engines available");
        fx_label(ui, "Typio could not find any keyboard engine to display right now.");
    } else {
        for (guint i = 0; i < order_count; i++) {
            const char *engine_name = ptr_array_get(settings->engine_order, i);
            char *status;

            if (!engine_name || !*engine_name)
                continue;

            status = engine_order_status(settings, engine_name);
            fx_row(ui);
            fx_column_ex(ui, (fx_layout_opts){ .cross = FX_START });
            fx_label_ex(ui, engine_label_fallback(engine_name), 16);
            fx_label(ui, status);
            fx_end(ui);
            fx_flex(ui, 1.0f);

            if (i > 0) {
                if (fx_button(ui, "Up"))
                    on_engine_order_move_up_clicked(settings, (int)i);
            }
            if (i + 1 < order_count) {
                if (fx_button(ui, "Down"))
                    on_engine_order_move_down_clicked(settings, (int)i);
            }
            if (!engine_order_is_active(settings, engine_name)) {
                if (fx_button(ui, "Activate"))
                    on_engine_activate_clicked(settings, (int)i);
            }
            if (fx_button(ui, "Edit"))
                on_engine_settings_edit_clicked(settings, (int)i);
            if (fx_button(ui, "Remove"))
                on_engine_order_remove_clicked(settings, (int)i);
            fx_end(ui);

            g_free(status);
        }
    }

    /* Add engine row */
    available_count = ptr_array_len(settings->engine_available_labels);
    if (available_count > 0) {
        fx_row(ui);
        fx_dropdown(ui, "Add engine", &settings->engine_order_add_selected,
                    (const char **)settings->engine_available_labels->pdata,
                    available_count);
        if (fx_button(ui, "Add to order"))
            on_engine_order_add_clicked(settings);
        if (settings->engine_order_has_custom) {
            if (fx_button(ui, "Reset"))
                on_engine_order_reset_clicked(settings);
        }
        fx_end(ui);
    }

    fx_label(ui, "Use the buttons to reorder. Use the pencil button to open settings for a specific engine.");

    /* Engine settings inline section */
    if (settings->engine_settings_open && settings->engine_settings_engine) {
        char *title;

        fx_separator(ui);
        title = g_strdup_printf("%s settings",
                                engine_label_fallback(settings->engine_settings_engine));
        fx_heading(ui, title, 2);
        g_free(title);

        if (g_strcmp0(settings->engine_settings_engine, "basic") == 0) {
            build_basic_config(ui, settings);
        } else if (g_strcmp0(settings->engine_settings_engine, "rime") == 0) {
            build_rime_config(ui, settings);
        } else if (g_strcmp0(settings->engine_settings_engine, "mozc") == 0) {
            build_mozc_config(ui, settings);
        } else {
            fx_label(ui, "This engine has no configurable options.");
        }

        if (fx_button(ui, "Close"))
            on_engine_settings_window_close_request(settings);
    }

    /* --- Voice input --- */
    fx_separator(ui);
    fx_heading(ui, "Voice input", 2);
    fx_label(ui, "Voice backends run alongside keyboard input. They do not replace the active keyboard engine, but only one voice backend is active at a time.");

    /* Voice backend */
    if (ptr_array_len(settings->voice_backend_ids) > 0) {
        fx_row(ui);
        pref_label_column(ui, "Voice backend",
            "Switch between Whisper and sherpa-onnx when both are available.");
        fx_flex(ui, 1.0f);
        if (fx_dropdown(ui, "", &settings->voice_backend_selected,
                        (const char **)settings->voice_backend_ids->pdata,
                        ptr_array_len(settings->voice_backend_ids))) {
            on_voice_backend_changed(settings, settings->voice_backend_selected);
        }
        fx_end(ui);
    }

    /* Voice model */
    if (ptr_array_len(settings->voice_model_names) > 0) {
        fx_row(ui);
        pref_label_column(ui, "Installed model",
            "Choose the local voice model used by the selected backend.");
        fx_flex(ui, 1.0f);
        if (fx_dropdown(ui, "", &settings->voice_model_selected,
                        (const char **)settings->voice_model_names->pdata,
                        ptr_array_len(settings->voice_model_names))) {
            on_display_dropdown_changed(settings, settings->voice_model_selected);
        }
        fx_end(ui);
    }

    backend_id = ptr_array_get(settings->voice_backend_ids, settings->voice_backend_selected);
    if (g_strcmp0(backend_id, "whisper") == 0) {
        settings_build_whisper_model_section(ui, settings);
    } else if (g_strcmp0(backend_id, "sherpa-onnx") == 0) {
        settings_build_sherpa_model_section(ui, settings);
    }

    fx_end(ui); /* column */
    fx_scroll_end(ui);
}

/* -------------------------------------------------------------------------- */
/* Shortcuts page                                                             */
/* -------------------------------------------------------------------------- */

static void build_shortcuts_page(fx_ui *ui, TypioSettings *settings)
{
    fx_scroll_begin(ui, "shortcuts-page");
    fx_column(ui);

    fx_heading(ui, "Shortcuts", 2);
    fx_label(ui, "Record combinations directly from the keyboard. Press Esc to cancel.");

    /* Switch language */
    fx_row(ui);
    pref_label_column(ui, "Switch language",
        "Cycle the enabled languages without opening the panel.");
    fx_flex(ui, 1.0f);
    if (settings->recording_shortcut && settings->recording_shortcut_index == 0) {
        if (fx_button(ui, "Press shortcut...")) {
            settings->recording_shortcut = false;
            settings->recording_shortcut_index = -1;
            settings_sync_form_from_buffer(settings);
        }
    } else {
        if (fx_button(ui, settings->shortcut_switch_language ? settings->shortcut_switch_language : "Ctrl+Shift")) {
            if (settings->recording_shortcut)
                settings_sync_form_from_buffer(settings);
            settings->recording_shortcut = true;
            settings->recording_shortcut_index = 0;
        }
    }
    fx_end(ui);

    /* Emergency exit */
    fx_row(ui);
    pref_label_column(ui, "Emergency exit",
        "Immediately stop Typio and release settings when input routing misbehaves.");
    fx_flex(ui, 1.0f);
    if (settings->recording_shortcut && settings->recording_shortcut_index == 1) {
        if (fx_button(ui, "Press shortcut...")) {
            settings->recording_shortcut = false;
            settings->recording_shortcut_index = -1;
            settings_sync_form_from_buffer(settings);
        }
    } else {
        if (fx_button(ui, settings->shortcut_emergency_exit ? settings->shortcut_emergency_exit : "Ctrl+Shift+Escape")) {
            if (settings->recording_shortcut)
                settings_sync_form_from_buffer(settings);
            settings->recording_shortcut = true;
            settings->recording_shortcut_index = 1;
        }
    }
    fx_end(ui);

    /* Voice push-to-talk */
    fx_row(ui);
    pref_label_column(ui, "Voice push-to-talk",
        "Hold the shortcut to activate the configured voice backend.");
    fx_flex(ui, 1.0f);
    if (settings->recording_shortcut && settings->recording_shortcut_index == 2) {
        if (fx_button(ui, "Press shortcut...")) {
            settings->recording_shortcut = false;
            settings->recording_shortcut_index = -1;
            settings_sync_form_from_buffer(settings);
        }
    } else {
        if (fx_button(ui, settings->shortcut_voice_ptt ? settings->shortcut_voice_ptt : "Super+v")) {
            if (settings->recording_shortcut)
                settings_sync_form_from_buffer(settings);
            settings->recording_shortcut = true;
            settings->recording_shortcut_index = 2;
        }
    }
    fx_end(ui);

    fx_label(ui, "Click a shortcut button, then press the new key combination.");

    fx_end(ui); /* column */
    fx_scroll_end(ui);
}

/* -------------------------------------------------------------------------- */
/* Main UI builder                                                            */
/* -------------------------------------------------------------------------- */

void settings_build_ui(fx_ui *ui, TypioSettings *settings)
{
    fx_column(ui);

    fx_tabs_begin(ui, "main-tabs", &settings->active_tab);
    fx_tab(ui, "Appearance");
    fx_tab(ui, "Input engines");
    fx_tab(ui, "Shortcuts");
    fx_tabs_end(ui);

    switch (settings->active_tab) {
    case 0:
        build_display_page(ui, settings);
        break;
    case 1:
        build_engines_page(ui, settings);
        break;
    case 2:
        build_shortcuts_page(ui, settings);
        break;
    default:
        settings->active_tab = 0;
        break;
    }

    if (settings->status_visible && settings->status_message && *settings->status_message) {
        fx_separator(ui);
        fx_label(ui, settings->status_message);
    }

    fx_end(ui);
}
