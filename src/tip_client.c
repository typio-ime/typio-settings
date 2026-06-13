/**
 * @file tip_client.c
 * @brief TIP v1 UDS client (ADR-0003).
 *
 * Hand-rolled JSON parser/encoder for the shapes this client exchanges:
 *   - request:  {"jsonrpc":"2.0","id":N,"method":"...","params":{...}}
 *   - response: {"jsonrpc":"2.0","id":N,"result":VALUE} or "error":...
 *   - event:    {"jsonrpc":"2.0","method":"<topic>","params":VALUE}
 *
 * The control connection is blocking; the event connection is wrapped in
 * a GSocket source so notifications fire from the GLib main loop.
 */

#include "tip_client.h"

#include <arpa/inet.h>
#include <errno.h>
#include <gio/gio.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define TIP_MAX_FRAME (1U << 20) /* 1 MiB */

/* TIP v1 method names (mirrors typio-wayland src/ipc/tip_protocol.h). */
#define TIP_METHOD_HELLO            "hello"
#define TIP_METHOD_CONFIG_SET       "config.set"
#define TIP_METHOD_CONFIG_RELOAD    "config.reload"
#define TIP_METHOD_CONFIG_SHOW      "config.show"
#define TIP_METHOD_ENGINE_LIST      "engine.list"
#define TIP_METHOD_ENGINE_USE       "engine.use"
#define TIP_METHOD_ENGINE_NEXT      "engine.next"
#define TIP_METHOD_ENGINE_INVOKE    "engine.invoke"
#define TIP_METHOD_LANGUAGE_LIST    "language.list"
#define TIP_METHOD_LANGUAGE_USE     "language.use"
#define TIP_METHOD_DAEMON_STATUS    "daemon.status"

/* TIP v1 topic names. */
#define TIP_TOPIC_ENGINE_CHANGED      "engine.changed"
#define TIP_TOPIC_ENGINE_STATUS_CHANGED "engine.statusChanged"
#define TIP_TOPIC_LANGUAGE_CHANGED    "language.changed"
#define TIP_TOPIC_CONFIG_CHANGED      "config.changed"
#define TIP_TOPIC_RUNTIME_CHANGED     "runtime.changed"
#define TIP_TOPIC_DAEMON_SHUTDOWN     "daemon.shuttingDown"

struct TipClient {
    int ctl_fd;
    int evt_fd;
    GSource *evt_source;
    GIOChannel *evt_channel;
    int next_id;

    TipSnapshot snapshot;
    TipSnapshotChangedCallback changed_cb;
    void *changed_user_data;

    /* Reassembly buffer for the event socket. */
    GByteArray *evt_buffer;
};

/* ---------------------------------------------------------------- */
/* Snapshot helpers                                                 */
/* ---------------------------------------------------------------- */

static void engine_entry_free(gpointer data)
{
    TipEngineEntry *e = data;
    if (!e) return;
    g_free(e->name);
    g_free(e->kind);
    g_free(e->display_name);
    g_free(e);
}

static void snapshot_clear(TipSnapshot *s)
{
    g_clear_pointer(&s->daemon_version, g_free);
    g_clear_pointer(&s->active_keyboard, g_free);
    g_clear_pointer(&s->active_voice, g_free);
    g_clear_pointer(&s->active_language, g_free);
    g_clear_pointer(&s->config_text, g_free);
    if (s->languages) g_ptr_array_set_size(s->languages, 0);
    if (s->engines) g_ptr_array_set_size(s->engines, 0);
    s->connected = false;
    s->protocol_version = 0;
}

static void snapshot_notify(TipClient *c)
{
    if (c->changed_cb) c->changed_cb(c, c->changed_user_data);
}

/* ---------------------------------------------------------------- */
/* UDS framing                                                      */
/* ---------------------------------------------------------------- */

static char *resolve_socket_path(void)
{
    const char *runtime = g_getenv("XDG_RUNTIME_DIR");
    if (runtime && *runtime)
        return g_build_filename(runtime, "typio", "daemon.sock", NULL);
    const char *home = g_getenv("HOME");
    if (home && *home)
        return g_build_filename(home, ".local", "share", "typio",
                                "daemon.sock", NULL);
    return g_strdup("/tmp/typio-daemon.sock");
}

static int uds_connect(GError **error)
{
    char *path = resolve_socket_path();
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "socket() failed: %s", g_strerror(errno));
        g_free(path);
        return -1;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    g_strlcpy(addr.sun_path, path, sizeof(addr.sun_path));
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "connect %s: %s", path, g_strerror(errno));
        close(fd);
        g_free(path);
        return -1;
    }
    g_free(path);
    return fd;
}

static bool write_frame(int fd, const char *json, GError **error)
{
    size_t len = strlen(json);
    if (len > TIP_MAX_FRAME) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "frame too large");
        return false;
    }
    uint32_t be = htonl((uint32_t)len);
    if (send(fd, &be, 4, MSG_NOSIGNAL) != 4
        || send(fd, json, len, MSG_NOSIGNAL) != (ssize_t)len) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "send: %s", g_strerror(errno));
        return false;
    }
    return true;
}

static char *read_frame_blocking(int fd, GError **error)
{
    uint32_t be;
    ssize_t n;
    size_t got = 0;
    while (got < 4) {
        n = recv(fd, ((char *)&be) + got, 4 - got, 0);
        if (n <= 0) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "recv: %s", g_strerror(errno));
            return NULL;
        }
        got += (size_t)n;
    }
    size_t len = ntohl(be);
    if (len > TIP_MAX_FRAME) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "oversized frame");
        return NULL;
    }
    char *buf = g_malloc(len + 1);
    got = 0;
    while (got < len) {
        n = recv(fd, buf + got, len - got, 0);
        if (n <= 0) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "recv body: %s", g_strerror(errno));
            g_free(buf);
            return NULL;
        }
        got += (size_t)n;
    }
    buf[len] = '\0';
    return buf;
}

/* ---------------------------------------------------------------- */
/* Minimal JSON helpers                                             */
/* ---------------------------------------------------------------- */

/* Skip whitespace. */
static const char *json_skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

/* Find the start of the value for "key" in a top-level JSON object.
 * Returns pointer to first non-ws char of value, or NULL. Brace/quote
 * aware but does not handle deeply nested escapes; that's fine for our
 * shapes. */
static const char *json_find(const char *obj, const char *key)
{
    if (!obj || !key) return NULL;
    const char *p = obj;
    p = json_skip_ws(p);
    if (*p != '{') return NULL;
    p++;
    size_t klen = strlen(key);
    while (*p) {
        p = json_skip_ws(p);
        if (*p == '}') return NULL;
        if (*p != '"') return NULL;
        p++;
        const char *kstart = p;
        while (*p && *p != '"') {
            if (*p == '\\' && p[1]) p++;
            p++;
        }
        if (!*p) return NULL;
        size_t this_klen = (size_t)(p - kstart);
        p++;                       /* past closing quote */
        p = json_skip_ws(p);
        if (*p != ':') return NULL;
        p++;
        p = json_skip_ws(p);
        if (this_klen == klen && memcmp(kstart, key, klen) == 0)
            return p;
        /* skip value: scan to matching delimiter at depth 0 */
        int depth = 0;
        bool in_str = false;
        while (*p) {
            if (in_str) {
                if (*p == '\\' && p[1]) p++;
                else if (*p == '"') in_str = false;
            } else {
                if (*p == '"') in_str = true;
                else if (*p == '{' || *p == '[') depth++;
                else if (*p == '}' || *p == ']') {
                    if (depth == 0) break;
                    depth--;
                } else if (*p == ',' && depth == 0) {
                    p++;
                    goto next_key;
                }
            }
            p++;
        }
        if (*p == '}') return NULL;
next_key:
        continue;
    }
    return NULL;
}

/* If `val` points at a JSON string, return a newly-allocated unescaped
 * copy. Handles \", \\, \n, \t, \r, \/, \b, \f. Returns NULL on type
 * mismatch. */
static char *json_dup_string(const char *val)
{
    if (!val) return NULL;
    val = json_skip_ws(val);
    if (*val != '"') return NULL;
    val++;
    GString *out = g_string_new(NULL);
    while (*val && *val != '"') {
        if (*val == '\\' && val[1]) {
            switch (val[1]) {
            case '"': g_string_append_c(out, '"'); break;
            case '\\': g_string_append_c(out, '\\'); break;
            case '/': g_string_append_c(out, '/'); break;
            case 'n': g_string_append_c(out, '\n'); break;
            case 't': g_string_append_c(out, '\t'); break;
            case 'r': g_string_append_c(out, '\r'); break;
            case 'b': g_string_append_c(out, '\b'); break;
            case 'f': g_string_append_c(out, '\f'); break;
            default:  g_string_append_c(out, val[1]); break;
            }
            val += 2;
        } else {
            g_string_append_c(out, *val++);
        }
    }
    return g_string_free(out, FALSE);
}

static bool json_get_bool(const char *val, bool dflt)
{
    if (!val) return dflt;
    val = json_skip_ws(val);
    if (strncmp(val, "true", 4) == 0) return true;
    if (strncmp(val, "false", 5) == 0) return false;
    return dflt;
}

static int json_get_int(const char *val, int dflt)
{
    if (!val) return dflt;
    val = json_skip_ws(val);
    char *end = NULL;
    long v = strtol(val, &end, 10);
    if (end == val) return dflt;
    return (int)v;
}

/* Iterate elements of a JSON array. Returns pointer to first element or
 * NULL. Update `*cursor` between calls; sets it to NULL when done. */
static const char *json_array_first(const char *arr, const char **cursor)
{
    *cursor = NULL;
    if (!arr) return NULL;
    arr = json_skip_ws(arr);
    if (*arr != '[') return NULL;
    arr++;
    arr = json_skip_ws(arr);
    if (*arr == ']') return NULL;
    *cursor = arr;
    return arr;
}

static const char *json_array_next(const char **cursor)
{
    if (!cursor || !*cursor) return NULL;
    const char *p = *cursor;
    int depth = 0;
    bool in_str = false;
    while (*p) {
        if (in_str) {
            if (*p == '\\' && p[1]) p++;
            else if (*p == '"') in_str = false;
        } else {
            if (*p == '"') in_str = true;
            else if (*p == '{' || *p == '[') depth++;
            else if (*p == '}' || *p == ']') {
                if (depth == 0) { *cursor = NULL; return NULL; }
                depth--;
            } else if (*p == ',' && depth == 0) {
                p++;
                p = json_skip_ws(p);
                if (*p == ']') { *cursor = NULL; return NULL; }
                *cursor = p;
                return p;
            }
        }
        p++;
    }
    *cursor = NULL;
    return NULL;
}

/* Append a JSON-escaped string literal (including surrounding quotes). */
static void json_append_escaped_string(GString *out, const char *s)
{
    g_string_append_c(out, '"');
    for (const char *p = s; p && *p; p++) {
        switch (*p) {
        case '"': g_string_append(out, "\\\""); break;
        case '\\': g_string_append(out, "\\\\"); break;
        case '\n': g_string_append(out, "\\n"); break;
        case '\t': g_string_append(out, "\\t"); break;
        case '\r': g_string_append(out, "\\r"); break;
        default:
            if ((unsigned char)*p < 0x20)
                g_string_append_printf(out, "\\u%04x", *p);
            else
                g_string_append_c(out, *p);
        }
    }
    g_string_append_c(out, '"');
}

/* ---------------------------------------------------------------- */
/* Request / response                                               */
/* ---------------------------------------------------------------- */

char *tip_client_call(TipClient *c, const char *method,
                       const char *params_json, GError **error)
{
    if (!c || c->ctl_fd < 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED,
                    "not connected");
        return NULL;
    }
    int id = c->next_id++;
    GString *req = g_string_new(NULL);
    g_string_append_printf(req, "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":",
                           id);
    json_append_escaped_string(req, method);
    g_string_append_printf(req, ",\"params\":%s}",
                           params_json ? params_json : "{}");
    bool ok = write_frame(c->ctl_fd, req->str, error);
    g_string_free(req, TRUE);
    if (!ok) return NULL;

    char *resp = read_frame_blocking(c->ctl_fd, error);
    if (!resp) return NULL;

    /* If response contains "error", surface its message and return NULL. */
    const char *err = json_find(resp, "error");
    if (err) {
        const char *msg = json_find(err, "message");
        char *msg_str = msg ? json_dup_string(msg) : g_strdup("RPC error");
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s",
                    msg_str ? msg_str : "RPC error");
        g_free(msg_str);
        g_free(resp);
        return NULL;
    }
    const char *result = json_find(resp, "result");
    if (!result) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "missing result");
        g_free(resp);
        return NULL;
    }
    /* Slice the result value out of the response. */
    const char *start = json_skip_ws(result);
    const char *end = start;
    int depth = 0;
    bool in_str = false;
    while (*end) {
        if (in_str) {
            if (*end == '\\' && end[1]) end++;
            else if (*end == '"') in_str = false;
        } else {
            if (*end == '"') in_str = true;
            else if (*end == '{' || *end == '[') depth++;
            else if (*end == '}' || *end == ']') {
                if (depth == 0) break;
                depth--;
            } else if ((*end == ',' || *end == '}') && depth == 0) break;
        }
        end++;
    }
    char *out = g_strndup(start, (size_t)(end - start));
    g_free(resp);
    return out;
}

/* ---------------------------------------------------------------- */
/* Snapshot refresh                                                 */
/* ---------------------------------------------------------------- */

static void parse_engine_list(TipClient *c, const char *arr_json)
{
    g_ptr_array_set_size(c->snapshot.engines, 0);
    const char *cur = NULL;
    for (const char *e = json_array_first(arr_json, &cur);
         e; e = json_array_next(&cur)) {
        TipEngineEntry *entry = g_new0(TipEngineEntry, 1);
        const char *v;
        if ((v = json_find(e, "name"))) entry->name = json_dup_string(v);
        if ((v = json_find(e, "kind"))) entry->kind = json_dup_string(v);
        if ((v = json_find(e, "displayName")))
            entry->display_name = json_dup_string(v);
        if ((v = json_find(e, "active"))) entry->active = json_get_bool(v, false);
        g_ptr_array_add(c->snapshot.engines, entry);
    }
}

void tip_client_refresh_snapshot(TipClient *c)
{
    if (!c) return;
    GError *err = NULL;

    char *hello = tip_client_call(c, TIP_METHOD_HELLO, "{}", &err);
    if (hello) {
        const char *v;
        if ((v = json_find(hello, "protocolVersion")))
            c->snapshot.protocol_version = json_get_int(v, 0);
        if ((v = json_find(hello, "daemonVersion"))) {
            g_free(c->snapshot.daemon_version);
            c->snapshot.daemon_version = json_dup_string(v);
        }
        c->snapshot.connected = true;
        g_free(hello);
    } else {
        c->snapshot.connected = false;
        g_clear_error(&err);
    }

    char *engines = tip_client_call(c, TIP_METHOD_ENGINE_LIST, "{}",
                                    &err);
    if (engines) {
        parse_engine_list(c, engines);
        g_free(engines);
    } else {
        g_clear_error(&err);
    }

    /* language.list needs protocolVersion >= 3; older daemons answer
     * "method not found", which we treat as "no languages". */
    g_ptr_array_set_size(c->snapshot.languages, 0);
    g_clear_pointer(&c->snapshot.active_language, g_free);
    if (c->snapshot.protocol_version >= 3) {
        char *langs = tip_client_call(c, TIP_METHOD_LANGUAGE_LIST, "{}",
                                      &err);
        if (langs) {
            const char *v;
            if ((v = json_find(langs, "active")))
                c->snapshot.active_language = json_dup_string(v);
            const char *arr = json_find(langs, "languages");
            const char *cur = NULL;
            for (const char *e = arr ? json_array_first(arr, &cur) : NULL;
                 e; e = json_array_next(&cur)) {
                const char *tag = json_find(e, "tag");
                if (tag)
                    g_ptr_array_add(c->snapshot.languages,
                                    json_dup_string(tag));
            }
            g_free(langs);
        } else {
            g_clear_error(&err);
        }
    }

    char *status = tip_client_call(c, TIP_METHOD_DAEMON_STATUS, "{}",
                                   &err);
    if (status) {
        const char *v;
        if ((v = json_find(status, "activeKeyboardEngine"))) {
            g_free(c->snapshot.active_keyboard);
            c->snapshot.active_keyboard = json_dup_string(v);
        }
        if ((v = json_find(status, "activeVoiceEngine"))) {
            g_free(c->snapshot.active_voice);
            c->snapshot.active_voice = json_dup_string(v);
        }
        g_free(status);
    } else {
        g_clear_error(&err);
    }

    char *show = tip_client_call(c, TIP_METHOD_CONFIG_SHOW, "{}", &err);
    if (show) {
        const char *v = json_find(show, "text");
        g_free(c->snapshot.config_text);
        c->snapshot.config_text = v ? json_dup_string(v) : NULL;
        g_free(show);
    } else {
        g_clear_error(&err);
    }

    snapshot_notify(c);
}

/* ---------------------------------------------------------------- */
/* Event source                                                     */
/* ---------------------------------------------------------------- */

static void handle_event(TipClient *c, const char *frame)
{
    const char *method_v = json_find(frame, "method");
    char *method = method_v ? json_dup_string(method_v) : NULL;
    if (!method) return;

    if (strcmp(method, TIP_TOPIC_ENGINE_CHANGED) == 0
        || strcmp(method, TIP_TOPIC_ENGINE_STATUS_CHANGED) == 0
        || strcmp(method, TIP_TOPIC_LANGUAGE_CHANGED) == 0
        || strcmp(method, TIP_TOPIC_CONFIG_CHANGED) == 0
        || strcmp(method, TIP_TOPIC_RUNTIME_CHANGED) == 0) {
        /* Refresh affected snapshot pieces. Engine and config changes
         * are coarse enough that a full re-read is cheap. */
        tip_client_refresh_snapshot(c);
    } else if (strcmp(method, TIP_TOPIC_DAEMON_SHUTDOWN) == 0) {
        c->snapshot.connected = false;
        snapshot_notify(c);
    }
    g_free(method);
}

static gboolean evt_source_cb(GIOChannel *source,
                              GIOCondition condition,
                              gpointer user_data)
{
    TipClient *c = user_data;
    if (condition & (G_IO_HUP | G_IO_ERR)) {
        c->snapshot.connected = false;
        snapshot_notify(c);
        return G_SOURCE_REMOVE;
    }
    int fd = g_io_channel_unix_get_fd(source);
    char buf[4096];
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) {
        c->snapshot.connected = false;
        snapshot_notify(c);
        return G_SOURCE_REMOVE;
    }
    g_byte_array_append(c->evt_buffer, (guint8 *)buf, (guint)n);

    while (c->evt_buffer->len >= 4) {
        uint32_t be;
        memcpy(&be, c->evt_buffer->data, 4);
        size_t len = ntohl(be);
        if (len > TIP_MAX_FRAME) {
            g_warning("tip_client: oversized event frame");
            c->snapshot.connected = false;
            snapshot_notify(c);
            return G_SOURCE_REMOVE;
        }
        if (c->evt_buffer->len < 4 + len) break;
        char *frame = g_strndup((const char *)(c->evt_buffer->data + 4), len);
        handle_event(c, frame);
        g_free(frame);
        g_byte_array_remove_range(c->evt_buffer, 0, (guint)(4 + len));
    }
    return G_SOURCE_CONTINUE;
}

void tip_client_subscribe_all(TipClient *c)
{
    if (!c || c->evt_fd < 0) return;
    /* events.subscribe with no topics = wildcard. The server's response
     * is `{"subscribed":true}` — read and discard. */
    write_frame(c->evt_fd,
                "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"events.subscribe\","
                "\"params\":{}}", NULL);
    char *first = read_frame_blocking(c->evt_fd, NULL);
    g_free(first);
}

/* ---------------------------------------------------------------- */
/* Lifecycle                                                        */
/* ---------------------------------------------------------------- */

TipClient *tip_client_new(void)
{
    TipClient *c = g_new0(TipClient, 1);
    c->ctl_fd = -1;
    c->evt_fd = -1;
    c->next_id = 1;
    c->snapshot.engines = g_ptr_array_new_with_free_func(engine_entry_free);
    c->snapshot.languages = g_ptr_array_new_with_free_func(g_free);
    c->evt_buffer = g_byte_array_new();
    return c;
}

bool tip_client_connect(TipClient *c, GError **error)
{
    if (!c) return false;
    if (c->ctl_fd >= 0) close(c->ctl_fd);
    if (c->evt_fd >= 0) close(c->evt_fd);
    c->ctl_fd = uds_connect(error);
    if (c->ctl_fd < 0) return false;
    c->evt_fd = uds_connect(error);
    if (c->evt_fd < 0) { close(c->ctl_fd); c->ctl_fd = -1; return false; }

    tip_client_subscribe_all(c);

    c->evt_channel = g_io_channel_unix_new(c->evt_fd);
    g_io_channel_set_encoding(c->evt_channel, NULL, NULL);
    g_io_channel_set_buffered(c->evt_channel, FALSE);
    guint id = g_io_add_watch(c->evt_channel,
                              G_IO_IN | G_IO_HUP | G_IO_ERR,
                              evt_source_cb, c);
    (void)id;

    tip_client_refresh_snapshot(c);
    return true;
}

void tip_client_destroy(TipClient *c)
{
    if (!c) return;
    if (c->evt_channel) {
        g_io_channel_shutdown(c->evt_channel, FALSE, NULL);
        g_io_channel_unref(c->evt_channel);
    }
    if (c->ctl_fd >= 0) close(c->ctl_fd);
    if (c->evt_fd >= 0) close(c->evt_fd);
    snapshot_clear(&c->snapshot);
    if (c->snapshot.engines) g_ptr_array_unref(c->snapshot.engines);
    if (c->snapshot.languages) g_ptr_array_unref(c->snapshot.languages);
    if (c->evt_buffer) g_byte_array_unref(c->evt_buffer);
    g_free(c);
}

const TipSnapshot *tip_client_snapshot(const TipClient *c)
{
    return c ? &c->snapshot : NULL;
}

void tip_client_set_changed_callback(TipClient *c,
                                      TipSnapshotChangedCallback cb,
                                      void *user_data)
{
    if (!c) return;
    c->changed_cb = cb;
    c->changed_user_data = user_data;
}

/* ---------------------------------------------------------------- */
/* High-level wrappers                                              */
/* ---------------------------------------------------------------- */

bool tip_engine_use(TipClient *c, const char *name, GError **error)
{
    GString *p = g_string_new("{\"name\":");
    json_append_escaped_string(p, name ? name : "");
    g_string_append_c(p, '}');
    char *r = tip_client_call(c, TIP_METHOD_ENGINE_USE, p->str, error);
    g_string_free(p, TRUE);
    g_free(r);
    return r != NULL;
}

bool tip_engine_next(TipClient *c, const char *kind, GError **error)
{
    GString *p = g_string_new("{");
    if (kind) {
        g_string_append(p, "\"kind\":");
        json_append_escaped_string(p, kind);
    }
    g_string_append_c(p, '}');
    char *r = tip_client_call(c, TIP_METHOD_ENGINE_NEXT, p->str, error);
    g_string_free(p, TRUE);
    g_free(r);
    return r != NULL;
}

bool tip_language_use(TipClient *c, const char *tag, GError **error)
{
    GString *p = g_string_new("{\"tag\":");
    json_append_escaped_string(p, tag ? tag : "");
    g_string_append_c(p, '}');
    char *r = tip_client_call(c, TIP_METHOD_LANGUAGE_USE, p->str, error);
    g_string_free(p, TRUE);
    g_free(r);
    return r != NULL;
}

bool tip_engine_invoke(TipClient *c, const char *name, const char *command,
                        GError **error)
{
    GString *p = g_string_new("{\"name\":");
    json_append_escaped_string(p, name ? name : "");
    g_string_append(p, ",\"command\":");
    json_append_escaped_string(p, command ? command : "");
    g_string_append_c(p, '}');
    char *r = tip_client_call(c, TIP_METHOD_ENGINE_INVOKE, p->str, error);
    g_string_free(p, TRUE);
    g_free(r);
    return r != NULL;
}

static bool config_set_str(TipClient *c, const char *key, const char *value,
                            GError **error)
{
    GString *p = g_string_new("{\"key\":");
    json_append_escaped_string(p, key);
    g_string_append(p, ",\"value\":");
    json_append_escaped_string(p, value ? value : "");
    g_string_append_c(p, '}');
    char *r = tip_client_call(c, TIP_METHOD_CONFIG_SET, p->str, error);
    g_string_free(p, TRUE);
    g_free(r);
    return r != NULL;
}

bool tip_config_set_string(TipClient *c, const char *key, const char *value,
                            GError **error)
{
    return config_set_str(c, key, value, error);
}

bool tip_config_set_int(TipClient *c, const char *key, int value,
                         GError **error)
{
    char buf[32];
    g_snprintf(buf, sizeof(buf), "%d", value);
    return config_set_str(c, key, buf, error);
}

bool tip_config_set_bool(TipClient *c, const char *key, bool value,
                          GError **error)
{
    return config_set_str(c, key, value ? "true" : "false", error);
}

char *tip_config_show(TipClient *c, GError **error)
{
    char *r = tip_client_call(c, TIP_METHOD_CONFIG_SHOW, "{}", error);
    if (!r) return NULL;
    const char *v = json_find(r, "text");
    char *text = v ? json_dup_string(v) : NULL;
    g_free(r);
    return text;
}

bool tip_config_reload(TipClient *c, GError **error)
{
    char *r = tip_client_call(c, TIP_METHOD_CONFIG_RELOAD, "{}", error);
    g_free(r);
    return r != NULL;
}
