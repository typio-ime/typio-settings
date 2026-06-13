/**
 * @file main.c
 * @brief flux-ui settings panel entry point.
 *
 * Wires the flux-ui immediate-mode frame loop to the TIP v1 UDS client
 * (ADR-0003). State changes flow over `events.subscribe` topics into the
 * snapshot-changed callback.
 */

#include "settings_internal.h"
#include "platform.h"

#include <getopt.h>
#include <stdio.h>
#include <string.h>

#ifndef TYPIO_SETTINGS_TEST
static gboolean settings_verbose_enabled = FALSE;

static gboolean settings_env_truthy(const char *value) {
    if (!value || !*value) return FALSE;
    return g_ascii_strcasecmp(value, "1") == 0
        || g_ascii_strcasecmp(value, "true") == 0
        || g_ascii_strcasecmp(value, "yes") == 0
        || g_ascii_strcasecmp(value, "on") == 0;
}

static void settings_enable_verbose_logging(void) {
    settings_verbose_enabled = TRUE;
    g_setenv("G_MESSAGES_DEBUG", "all", TRUE);
#if GLIB_CHECK_VERSION(2, 72, 0)
    g_log_set_debug_enabled(TRUE);
#endif
}

static void settings_print_help(const char *program_name) {
    printf("Usage: %s [OPTIONS]\n", program_name);
    printf("  -v, --verbose   Enable verbose settings-center logging\n");
    printf("  -h, --help      Show this help message\n");
}
#endif

#ifndef TYPIO_SETTINGS_TEST
static void on_snapshot_changed([[maybe_unused]] TipClient *client,
                                gpointer user_data) {
    settings_refresh_from_proxy((TypioSettings *)user_data);
}

static void try_reconnect(TypioSettings *settings) {
    if (!settings || !settings->client) return;
    const TipSnapshot *snap = tip_client_snapshot(settings->client);
    if (snap && snap->connected) return;

    GError *error = NULL;
    if (tip_client_connect(settings->client, &error)) {
        g_message("typio-settings: reconnected to daemon");
        settings_refresh_from_proxy(settings);
    }
    g_clear_error(&error);
}

void settings_clear_proxy(TypioSettings *settings) {
    if (!settings) return;
    if (settings->client) {
        tip_client_destroy(settings->client);
        settings->client = NULL;
    }
}

static void build_ui(fx_ui *ui, void *user_data) {
    TypioSettings *settings = user_data;
    if (!settings) return;

    /* Reconnect timer: check every ~2 seconds if disconnected. */
    if (settings->client) {
        const TipSnapshot *snap = tip_client_snapshot(settings->client);
        if (!snap || !snap->connected) {
            uint64_t now = typio_platform_monotonic_ms();
            if (now - settings->last_reconnect_time >= 2000) {
                settings->last_reconnect_time = now;
                try_reconnect(settings);
            }
        }
    }

    /* Autosave timer */
    if (settings->autosave_pending) {
        uint64_t now = typio_platform_monotonic_ms();
        if (now >= settings->autosave_deadline) {
            settings->autosave_pending = false;
            settings->autosave_deadline = 0;
            /* Trigger autosave */
            settings_sync_buffer_from_form(settings);
            if (settings_has_pending_config_change(settings)) {
                settings_queue_autosave(settings, settings->autosave_priority);
            }
        }
    }

    settings_build_ui(ui, settings);
}

int main(int argc, char **argv) {
    TypioSettings *settings;
    int write_idx = 1;

    if (settings_env_truthy(g_getenv("TYPIO_SETTINGS_VERBOSE")))
        settings_enable_verbose_logging();

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            settings_enable_verbose_logging();
            continue;
        }
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            settings_print_help(argv[0]);
            return 0;
        }
        argv[write_idx++] = argv[i];
    }
    argc = write_idx;

    if (settings_verbose_enabled)
        g_message("Typio Control verbose logging enabled");

    settings = settings_new();
    settings->client = tip_client_new();
    tip_client_set_changed_callback(settings->client, on_snapshot_changed,
                                     settings);

    GError *error = NULL;
    if (!tip_client_connect(settings->client, &error)) {
        g_warning("typio-settings: cannot reach typiod daemon: %s",
                  error ? error->message : "(unknown)");
        settings_update_availability_label(settings,
            "Typio daemon is not running (check `typiod` is started).",
            TRUE);
        g_clear_error(&error);
    }
    settings_refresh_from_proxy(settings);

    int status = typio_platform_run(&(typio_platform_config){
        .title = "Typio Control",
        .width = 920,
        .height = 680,
        .dark = true,
        .build = build_ui,
        .user = settings,
    });

    settings_clear_proxy(settings);
    settings_free(settings);
    return status;
}
#endif
