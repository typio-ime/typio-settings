/**
 * @file tip_client.h
 * @brief TIP v1 client for the settings panel (ADR-0003).
 *
 * Two long-lived UDS connections to `$XDG_RUNTIME_DIR/typio/daemon.sock`:
 *   - control socket — blocking request/response for RPC methods
 *     (`engine.use`, `config.set`, …)
 *   - event socket — `events.subscribe` stream; reads dispatch on the
 *     GLib main loop via a GSocket source
 *
 * The client owns a small typed snapshot (active engines, engine list,
 * raw config text) refreshed from the daemon and kept current by event
 * topics. UI code reads the snapshot rather than calling the daemon on
 * every property access.
 */

#ifndef TYPIO_TIP_CLIENT_H
#define TYPIO_TIP_CLIENT_H

#include <glib.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct TipClient TipClient;

/** Per-engine summary entry from `engine.list`. */
typedef struct TipEngineEntry {
    char *name;
    char *kind;         /* "keyboard" or "voice" */
    char *display_name;
    bool active;
} TipEngineEntry;

/** Snapshot of daemon state the UI reads against. */
typedef struct TipSnapshot {
    bool connected;
    int protocol_version;
    char *daemon_version;
    char *active_keyboard;
    char *active_voice;
    char *active_language;  /* BCP-47 tag, empty when none (TIP v3) */
    GPtrArray *languages;   /* of char* tag; enabled language cycle */
    GPtrArray *engines;     /* of TipEngineEntry* */
    char *config_text;      /* TOML; freshly snapshotted from config.show */
} TipSnapshot;

/** Fired when any part of the snapshot changes. */
typedef void (*TipSnapshotChangedCallback)(TipClient *client, void *user_data);

TipClient *tip_client_new(void);
void tip_client_destroy(TipClient *client);

/** Open both connections (control + event) and subscribe to all topics. */
bool tip_client_connect(TipClient *client, GError **error);

/** Subscribe to all events emitted by the daemon. */
void tip_client_subscribe_all(TipClient *client);

/** Force a snapshot refresh (called once after connect). */
void tip_client_refresh_snapshot(TipClient *client);

const TipSnapshot *tip_client_snapshot(const TipClient *client);

void tip_client_set_changed_callback(TipClient *client,
                                      TipSnapshotChangedCallback cb,
                                      void *user_data);

/* ---------------------------------------------------------------- */
/* Method calls                                                     */
/* ---------------------------------------------------------------- */

/**
 * @brief Synchronous RPC. Caller frees the returned JSON string.
 *
 * @param method     dotted TIP method name, e.g. `"engine.use"`.
 * @param params_json raw JSON for params (e.g. `"{\"name\":\"rime\"}"`).
 *                   Pass `"{}"` for no params.
 * @param error      GError on transport/protocol error.
 * @return malloc'd JSON value (the `result` field), or NULL on failure.
 */
char *tip_client_call(TipClient *client, const char *method,
                       const char *params_json, GError **error);

/* High-level convenience wrappers (return TRUE on success). */
bool tip_engine_use(TipClient *client, const char *name, GError **error);
bool tip_engine_next(TipClient *client, const char *kind /* or NULL */,
                      GError **error);
bool tip_engine_invoke(TipClient *client, const char *name,
                        const char *command, GError **error);

/** Activate a language by BCP-47 tag (TIP v3 `language.use`). */
bool tip_language_use(TipClient *client, const char *tag, GError **error);

bool tip_config_set_string(TipClient *client, const char *key,
                            const char *value, GError **error);
bool tip_config_set_int(TipClient *client, const char *key,
                         int value, GError **error);
bool tip_config_set_bool(TipClient *client, const char *key,
                          bool value, GError **error);

/* Returns a freshly-allocated TOML string from config.show, or NULL on error.
 * Caller frees with g_free(). */
char *tip_config_show(TipClient *client, GError **error);

bool tip_config_reload(TipClient *client, GError **error);

#ifdef __cplusplus
}
#endif

#endif /* TYPIO_TIP_CLIENT_H */
