/* wl_platform.c — native Wayland window + Vulkan (via flux) + fx_input.
 *
 * Replaces the GLFW glue the examples used to share. Pure Wayland:
 * xdg-shell for the toplevel, xdg-decoration for a server-side title bar
 * when available, xkbcommon for the keymap, and VK_KHR_wayland_surface to
 * hand flux core a VkSurfaceKHR. Pointer and keyboard events are folded
 * into one fx_input per frame (ADR-0006).
 */

#include "platform.h"

#include <flux/flux.h>
#include <flux/vulkan.h>

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_wayland.h>

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "text-input-unstable-v3-client-protocol.h"

#include <linux/input-event-codes.h>   /* BTN_LEFT / BTN_RIGHT / BTN_MIDDLE */

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <glib.h>

/* focus.c maps Tab from this portable keycode (ADR-0006). */
#define TYPIO_PLATFORM_KEY_TAB 258

/* ------------------------------------------------------------------ */
/*  Accumulated input, drained into one fx_input per frame             */
/* ------------------------------------------------------------------ */

typedef struct typio_accum {
    double   cx, cy;                      /* latest cursor, surface-local   */
    bool     down[FX_MOUSE_COUNT];        /* held state (persists)          */
    bool     pressed[FX_MOUSE_COUNT];     /* edges this frame               */
    bool     released[FX_MOUSE_COUNT];
    double   scroll_x, scroll_y;          /* wheel this frame               */
    uint32_t mods;                        /* level state (persists)         */
    char     text[32];                    /* committed text this frame      */
    char     preedit[FX_PREEDIT_MAX];     /* IME preedit string             */
    uint32_t preedit_cursor;              /* caret byte offset in preedit   */
    fx_key_event keys[FX_INPUT_MAX_KEYS];
    uint32_t key_count;
} typio_accum;

/* ------------------------------------------------------------------ */
/*  Platform state                                                     */
/* ------------------------------------------------------------------ */

/* One wl_output binding so we can read its scale. */
#define TYPIO_PLATFORM_MAX_OUTPUTS 8
typedef struct typio_output {
    struct wl_output *wl;
    int               scale;       /* output integer scale (1, 2, 3 …) */
    bool              entered;     /* surface currently on this output */
} typio_output;

typedef struct typio_platform {
    struct wl_display    *display;
    struct wl_registry   *registry;
    struct wl_compositor *compositor;
    struct xdg_wm_base   *wm_base;
    struct wl_seat       *seat;
    struct wl_pointer    *pointer;
    struct wl_keyboard   *keyboard;
    struct zxdg_decoration_manager_v1 *deco_mgr;

    struct wl_data_device_manager *data_device_mgr;
    struct wl_data_device         *data_device;
    struct wl_data_offer          *selection_offer; /* current clipboard offer */
    struct wl_data_source         *copy_source;     /* our outgoing selection  */
    char     *copy_buf;            /* text we currently advertise for copy     */
    size_t    copy_len;
    uint32_t  last_serial;         /* most recent input serial (set_selection) */

    struct wl_surface    *surface;
    struct xdg_surface   *xdg_surface;
    struct xdg_toplevel  *toplevel;
    struct zxdg_toplevel_decoration_v1 *deco;

    struct xkb_context   *xkb_ctx;
    struct xkb_keymap    *xkb_keymap;
    struct xkb_state     *xkb_state;

    struct zwp_text_input_manager_v3 *text_input_mgr;
    struct zwp_text_input_v3         *text_input;
    struct wl_surface                *text_input_surface;

    /* Pending IME state (double-buffered by text-input-v3 done event) */
    struct {
        char     commit[64];
        char     preedit[FX_PREEDIT_MAX];
        int32_t  preedit_cursor_begin;
        int32_t  preedit_cursor_end;
        uint32_t delete_before;
        uint32_t delete_after;
        bool     has_commit;
        bool     has_preedit;
        bool     has_delete;
    } ime;

    typio_output outputs[TYPIO_PLATFORM_MAX_OUTPUTS];
    int       n_outputs;
    int       buffer_scale;   /* max scale of entered outputs; ≥1     */
    int       pending_scale;  /* recomputed from enter/leave + globals */

    int  width, height;       /* surface size in *logical* pixels      */
    int  pending_w, pending_h;/* from the latest toplevel.configure    */
    bool running;
    bool resized;             /* size or scale changed -> resize swap  */
    fx_ui *ui;                /* so output/scale callbacks can update  */

    typio_accum acc;
} typio_platform;

/* Recompute the scale we should use: max integer scale across the outputs
 * the surface currently sits on. If none known yet, fall back to 1. */
static void typio_recompute_scale(typio_platform *pl)
{
    int s = 0;
    for (int i = 0; i < pl->n_outputs; i++)
        if (pl->outputs[i].entered && pl->outputs[i].scale > s)
            s = pl->outputs[i].scale;
    if (s <= 0) s = 1;
    pl->pending_scale = s;
}

/* ------------------------------------------------------------------ */
/*  xdg_wm_base — answer pings                                          */
/* ------------------------------------------------------------------ */

static void wm_base_ping(void *data, struct xdg_wm_base *b, uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(b, serial);
}
static const struct xdg_wm_base_listener wm_base_listener = { .ping = wm_base_ping };

/* ------------------------------------------------------------------ */
/*  Pointer                                                            */
/* ------------------------------------------------------------------ */

static int mouse_index(uint32_t button)
{
    switch (button) {
    case BTN_LEFT:   return FX_MOUSE_LEFT;
    case BTN_RIGHT:  return FX_MOUSE_RIGHT;
    case BTN_MIDDLE: return FX_MOUSE_MIDDLE;
    default:         return -1;
    }
}

static void ptr_enter(void *data, struct wl_pointer *p, uint32_t serial,
                      struct wl_surface *surf, wl_fixed_t x, wl_fixed_t y)
{
    (void)p; (void)serial; (void)surf;
    typio_platform *pl = data;
    pl->acc.cx = wl_fixed_to_double(x);
    pl->acc.cy = wl_fixed_to_double(y);
}
static void ptr_leave(void *data, struct wl_pointer *p, uint32_t serial,
                      struct wl_surface *surf)
{
    (void)p; (void)serial; (void)surf;
    typio_platform *pl = data;
    pl->acc.cx = pl->acc.cy = -100000.0;   /* off-window: clears hover */
}
static void ptr_motion(void *data, struct wl_pointer *p, uint32_t t,
                       wl_fixed_t x, wl_fixed_t y)
{
    (void)p; (void)t;
    typio_platform *pl = data;
    pl->acc.cx = wl_fixed_to_double(x);
    pl->acc.cy = wl_fixed_to_double(y);
}
static void ptr_button(void *data, struct wl_pointer *p, uint32_t serial,
                       uint32_t t, uint32_t button, uint32_t state)
{
    (void)p; (void)serial; (void)t;
    typio_platform *pl = data;
    int i = mouse_index(button);
    if (i < 0) return;
    bool down = (state == WL_POINTER_BUTTON_STATE_PRESSED);
    if (down && !pl->acc.down[i]) pl->acc.pressed[i]  = true;
    if (!down && pl->acc.down[i]) pl->acc.released[i] = true;
    pl->acc.down[i] = down;
}
static void ptr_axis(void *data, struct wl_pointer *p, uint32_t t,
                     uint32_t axis, wl_fixed_t value)
{
    (void)p; (void)t;
    typio_platform *pl = data;
    /* Wayland axis is positive down/right, ~10 units per notch. Normalise
     * to ~±1 per notch, positive = up, to match the old GLFW convention. */
    double v = wl_fixed_to_double(value) / 10.0;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)        pl->acc.scroll_y -= v;
    else if (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL) pl->acc.scroll_x -= v;
}
static void ptr_frame(void *d, struct wl_pointer *p) { (void)d; (void)p; }
static void ptr_axis_source(void *d, struct wl_pointer *p, uint32_t s)
{ (void)d; (void)p; (void)s; }
static void ptr_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a)
{ (void)d; (void)p; (void)t; (void)a; }
static void ptr_axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t v)
{ (void)d; (void)p; (void)a; (void)v; }
static void ptr_axis_value120(void *d, struct wl_pointer *p, uint32_t a, int32_t v)
{ (void)d; (void)p; (void)a; (void)v; }
static void ptr_axis_relative_direction(void *d, struct wl_pointer *p, uint32_t a, uint32_t dir)
{ (void)d; (void)p; (void)a; (void)dir; }

static const struct wl_pointer_listener pointer_listener = {
    .enter = ptr_enter, .leave = ptr_leave, .motion = ptr_motion,
    .button = ptr_button, .axis = ptr_axis, .frame = ptr_frame,
    .axis_source = ptr_axis_source, .axis_stop = ptr_axis_stop,
    .axis_discrete = ptr_axis_discrete, .axis_value120 = ptr_axis_value120,
    .axis_relative_direction = ptr_axis_relative_direction,
};

/* ------------------------------------------------------------------ */
/*  Keyboard (xkbcommon)                                               */
/* ------------------------------------------------------------------ */

static void kb_keymap(void *data, struct wl_keyboard *k, uint32_t format,
                      int32_t fd, uint32_t size)
{
    (void)k;
    typio_platform *pl = data;
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) { close(fd); return; }
    char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) { close(fd); return; }
    struct xkb_keymap *km = xkb_keymap_new_from_string(
        pl->xkb_ctx, map, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);
    close(fd);
    if (!km) return;
    struct xkb_state *st = xkb_state_new(km);
    if (!st) { xkb_keymap_unref(km); return; }
    if (pl->xkb_state)  xkb_state_unref(pl->xkb_state);
    if (pl->xkb_keymap) xkb_keymap_unref(pl->xkb_keymap);
    pl->xkb_keymap = km;
    pl->xkb_state  = st;
}
static void kb_enter(void *d, struct wl_keyboard *k, uint32_t s,
                     struct wl_surface *surf, struct wl_array *keys)
{
    (void)k; (void)s; (void)keys;
    typio_platform *pl = d;
    if (pl->text_input) {
        zwp_text_input_v3_enable(pl->text_input);
        zwp_text_input_v3_set_content_type(pl->text_input,
            ZWP_TEXT_INPUT_V3_CONTENT_HINT_NONE,
            ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NORMAL);
        zwp_text_input_v3_commit(pl->text_input);
    }
    (void)surf;
}
static void kb_leave(void *d, struct wl_keyboard *k, uint32_t s,
                     struct wl_surface *surf)
{
    (void)k; (void)s; (void)surf;
    typio_platform *pl = d;
    if (pl->text_input) {
        zwp_text_input_v3_disable(pl->text_input);
        zwp_text_input_v3_commit(pl->text_input);
    }
}

/* ------------------------------------------------------------------ */
/*  zwp_text_input_v3 listener (IME)                                  */
/* ------------------------------------------------------------------ */

static void ti_enter(void *data, struct zwp_text_input_v3 *ti,
                     struct wl_surface *surface)
{
    typio_platform *pl = data;
    (void)ti;
    pl->text_input_surface = surface;
}

static void ti_leave(void *data, struct zwp_text_input_v3 *ti,
                     struct wl_surface *surface)
{
    typio_platform *pl = data;
    (void)ti; (void)surface;
    pl->text_input_surface = NULL;
}

static void ti_preedit(void *data, struct zwp_text_input_v3 *ti,
                       const char *text, int32_t cursor_begin,
                       int32_t cursor_end)
{
    typio_platform *pl = data;
    (void)ti; (void)cursor_end;
    if (text) {
        strncpy(pl->ime.preedit, text, sizeof pl->ime.preedit - 1);
        pl->ime.preedit[sizeof pl->ime.preedit - 1] = '\0';
    } else {
        pl->ime.preedit[0] = '\0';
    }
    pl->ime.preedit_cursor_begin = cursor_begin;
    pl->ime.has_preedit = true;
}

static void ti_commit(void *data, struct zwp_text_input_v3 *ti,
                      const char *text)
{
    typio_platform *pl = data;
    (void)ti;
    if (text) {
        strncpy(pl->ime.commit, text, sizeof pl->ime.commit - 1);
        pl->ime.commit[sizeof pl->ime.commit - 1] = '\0';
    } else {
        pl->ime.commit[0] = '\0';
    }
    pl->ime.has_commit = true;
}

static void ti_delete_surrounding(void *data, struct zwp_text_input_v3 *ti,
                                  uint32_t before, uint32_t after)
{
    typio_platform *pl = data;
    (void)ti;
    pl->ime.delete_before = before;
    pl->ime.delete_after  = after;
    pl->ime.has_delete    = true;
}

static void ti_done(void *data, struct zwp_text_input_v3 *ti, uint32_t serial)
{
    typio_platform *pl = data;
    (void)ti; (void)serial;

    /* Apply pending IME state in the order specified by the protocol:
     * 1. Delete surrounding text
     * 2. Insert commit string
     * 3. Set preedit string */
    if (pl->ime.has_delete) {
        /* Best-effort: for now we ignore delete_surrounding_text because
         * flux-ui's fx_input has no corresponding field. The commit string
         * usually carries the corrected text anyway. */
    }

    if (pl->ime.has_commit && pl->ime.commit[0]) {
        size_t used = strlen(pl->acc.text);
        size_t n = strlen(pl->ime.commit);
        if (used + n < sizeof pl->acc.text)
            memcpy(pl->acc.text + used, pl->ime.commit, n + 1);
        pl->ime.commit[0] = '\0';
    }

    if (pl->ime.has_preedit) {
        if (pl->ime.preedit[0]) {
            strncpy(pl->acc.preedit, pl->ime.preedit, sizeof pl->acc.preedit - 1);
            pl->acc.preedit[sizeof pl->acc.preedit - 1] = '\0';
            pl->acc.preedit_cursor = pl->ime.preedit_cursor_begin >= 0
                ? (uint32_t)pl->ime.preedit_cursor_begin : 0;
        } else {
            pl->acc.preedit[0] = '\0';
            pl->acc.preedit_cursor = 0;
        }
    }

    pl->ime.has_commit  = false;
    pl->ime.has_preedit = false;
    pl->ime.has_delete  = false;
}

static const struct zwp_text_input_v3_listener text_input_listener = {
    .enter = ti_enter,
    .leave = ti_leave,
    .preedit_string = ti_preedit,
    .commit_string = ti_commit,
    .delete_surrounding_text = ti_delete_surrounding,
    .done = ti_done,
};

static void kb_key(void *data, struct wl_keyboard *k, uint32_t serial,
                   uint32_t t, uint32_t key, uint32_t state)
{
    (void)k; (void)t;
    typio_platform *pl = data;
    if (!pl->xkb_state) return;
    pl->last_serial = serial;   /* needed for wl_data_device_set_selection */
    bool pressed = (state == WL_KEYBOARD_KEY_STATE_PRESSED);
    xkb_keycode_t code = key + 8;   /* evdev -> xkb */
    xkb_keysym_t sym = xkb_state_key_get_one_sym(pl->xkb_state, code);

    if (sym == XKB_KEY_Escape && pressed) { pl->running = false; return; }

    if (pressed && pl->acc.key_count < FX_INPUT_MAX_KEYS) {
        int fk = 0;
        if (sym == XKB_KEY_Tab)        fk = FX_KEY_TAB;
        else if (sym == XKB_KEY_Return)fk = FX_KEY_RETURN;
        else if (sym == XKB_KEY_BackSpace) fk = FX_KEY_BACKSPACE;
        else if (sym == XKB_KEY_Delete)fk = FX_KEY_DELETE;
        else if (sym == XKB_KEY_Left)  fk = FX_KEY_LEFT;
        else if (sym == XKB_KEY_Right) fk = FX_KEY_RIGHT;
        else if (sym == XKB_KEY_Up)    fk = FX_KEY_UP;
        else if (sym == XKB_KEY_Down)  fk = FX_KEY_DOWN;
        else if (sym == XKB_KEY_Home)  fk = FX_KEY_HOME;
        else if (sym == XKB_KEY_End)   fk = FX_KEY_END;
        /* ASCII letters (and digits) so widgets see Ctrl+C/V/X/A etc.
         * xkb keysyms for these equal their ASCII codepoints. */
        else if (sym >= XKB_KEY_space && sym <= XKB_KEY_asciitilde)
            fk = (int)sym;
        if (fk) {
            pl->acc.keys[pl->acc.key_count++] = (fx_key_event){
                .key = fk, .pressed = true, .repeat = false };
        }
    }

    if (pressed) {
        char buf[8];
        int n = xkb_state_key_get_utf8(pl->xkb_state, code, buf, sizeof buf);
        if (n > 0 && (unsigned char)buf[0] >= 0x20) {  /* skip control chars */
            size_t used = strlen(pl->acc.text);
            if (used + (size_t)n < sizeof pl->acc.text)
                memcpy(pl->acc.text + used, buf, (size_t)n + 1);
        }
    }
}
static void kb_modifiers(void *data, struct wl_keyboard *k, uint32_t serial,
                         uint32_t dep, uint32_t latched, uint32_t locked,
                         uint32_t group)
{
    (void)k; (void)serial;
    typio_platform *pl = data;
    if (!pl->xkb_state) return;
    xkb_state_update_mask(pl->xkb_state, dep, latched, locked, 0, 0, group);
    struct { const char *name; uint32_t bit; } m[] = {
        { XKB_MOD_NAME_SHIFT, FX_MOD_SHIFT },
        { XKB_MOD_NAME_CTRL,  FX_MOD_CTRL  },
        { XKB_MOD_NAME_ALT,   FX_MOD_ALT   },
        { XKB_MOD_NAME_LOGO,  FX_MOD_SUPER },
    };
    for (size_t i = 0; i < sizeof m / sizeof m[0]; i++) {
        bool on = xkb_state_mod_name_is_active(
            pl->xkb_state, m[i].name, XKB_STATE_MODS_EFFECTIVE) > 0;
        if (on) pl->acc.mods |=  m[i].bit;
        else    pl->acc.mods &= ~m[i].bit;
    }
}
static void kb_repeat(void *d, struct wl_keyboard *k, int32_t rate, int32_t delay)
{ (void)d; (void)k; (void)rate; (void)delay; }

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = kb_keymap, .enter = kb_enter, .leave = kb_leave,
    .key = kb_key, .modifiers = kb_modifiers, .repeat_info = kb_repeat,
};

/* ------------------------------------------------------------------ */
/*  Clipboard (wl_data_device) — bridges fx_clipboard to the selection */
/* ------------------------------------------------------------------ */

/* A data offer we might read on paste; remember the most recent text one. */
static void doffer_offer(void *data, struct wl_data_offer *off, const char *mime)
{
    typio_platform *pl = data;
    (void)off;
    /* Track only that *some* text type is on offer; receive negotiates below. */
    if (strstr(mime, "text/")) { /* keep as the current selection candidate */ }
    (void)pl;
}
static void doffer_source_actions(void *d, struct wl_data_offer *o, uint32_t a)
{ (void)d; (void)o; (void)a; }
static void doffer_action(void *d, struct wl_data_offer *o, uint32_t a)
{ (void)d; (void)o; (void)a; }
static const struct wl_data_offer_listener data_offer_listener = {
    .offer = doffer_offer,
    .source_actions = doffer_source_actions,
    .action = doffer_action,
};

/* The compositor announces a new offer object before selection/enter. */
static void ddev_data_offer(void *data, struct wl_data_device *dev,
                            struct wl_data_offer *offer)
{
    (void)data; (void)dev;
    wl_data_offer_add_listener(offer, &data_offer_listener, data);
}
static void ddev_selection(void *data, struct wl_data_device *dev,
                           struct wl_data_offer *offer)
{
    typio_platform *pl = data;
    (void)dev;
    if (pl->selection_offer && pl->selection_offer != offer)
        wl_data_offer_destroy(pl->selection_offer);
    pl->selection_offer = offer;   /* may be NULL when selection is cleared */
}
static void ddev_enter(void *d, struct wl_data_device *dev, uint32_t s,
                       struct wl_surface *su, wl_fixed_t x, wl_fixed_t y,
                       struct wl_data_offer *o)
{ (void)d; (void)dev; (void)s; (void)su; (void)x; (void)y; (void)o; }
static void ddev_leave(void *d, struct wl_data_device *dev)
{ (void)d; (void)dev; }
static void ddev_motion(void *d, struct wl_data_device *dev, uint32_t t,
                        wl_fixed_t x, wl_fixed_t y)
{ (void)d; (void)dev; (void)t; (void)x; (void)y; }
static void ddev_drop(void *d, struct wl_data_device *dev)
{ (void)d; (void)dev; }
static const struct wl_data_device_listener data_device_listener = {
    .data_offer = ddev_data_offer,
    .enter = ddev_enter,
    .leave = ddev_leave,
    .motion = ddev_motion,
    .drop = ddev_drop,
    .selection = ddev_selection,
};

/* Our outgoing selection: write the stored buffer when a reader asks. */
static void dsource_send(void *data, struct wl_data_source *src,
                         const char *mime, int32_t fd)
{
    typio_platform *pl = data;
    (void)src; (void)mime;
    const char *p = pl->copy_buf;
    size_t left = pl->copy_len;
    while (left) {
        ssize_t w = write(fd, p, left);
        if (w <= 0) break;
        p += w; left -= (size_t)w;
    }
    close(fd);
}
static void dsource_cancelled(void *data, struct wl_data_source *src)
{
    typio_platform *pl = data;
    wl_data_source_destroy(src);
    if (pl->copy_source == src) pl->copy_source = NULL;
}
static void dsource_target(void *d, struct wl_data_source *s, const char *m)
{ (void)d; (void)s; (void)m; }
static const struct wl_data_source_listener data_source_listener = {
    .target = dsource_target,
    .send = dsource_send,
    .cancelled = dsource_cancelled,
};

static void typio_maybe_create_data_device(typio_platform *pl)
{
    if (pl->data_device || !pl->data_device_mgr || !pl->seat) return;
    pl->data_device = wl_data_device_manager_get_data_device(
        pl->data_device_mgr, pl->seat);
    wl_data_device_add_listener(pl->data_device, &data_device_listener, pl);
}

/* fx_clipboard.set_text — advertise `utf8` as the system selection. */
static void clip_set_text(const char *utf8, size_t len, void *user)
{
    typio_platform *pl = user;
    if (!pl->data_device_mgr || !pl->data_device) return;

    char *copy = malloc(len ? len : 1);
    if (!copy) return;
    memcpy(copy, utf8, len);
    free(pl->copy_buf);
    pl->copy_buf = copy;
    pl->copy_len = len;

    pl->copy_source = wl_data_device_manager_create_data_source(pl->data_device_mgr);
    wl_data_source_add_listener(pl->copy_source, &data_source_listener, pl);
    wl_data_source_offer(pl->copy_source, "text/plain;charset=utf-8");
    wl_data_source_offer(pl->copy_source, "text/plain");
    wl_data_source_offer(pl->copy_source, "UTF8_STRING");
    wl_data_device_set_selection(pl->data_device, pl->copy_source, pl->last_serial);
}

/* fx_clipboard.request_text — read the current selection and hand it back.
 * Reading is synchronous here (an example); we pump the display so our own
 * data source can answer when we are the selection owner. */
static void clip_request_text(void *user)
{
    typio_platform *pl = user;
    if (!pl->selection_offer || !pl->ui) return;

    int fds[2];
    if (pipe(fds) != 0) return;
    wl_data_offer_receive(pl->selection_offer, "text/plain;charset=utf-8", fds[1]);
    close(fds[1]);
    wl_display_flush(pl->display);

    char  *out = NULL;
    size_t total = 0;
    for (;;) {
        struct pollfd pfd = { .fd = fds[0], .events = POLLIN };
        int pr = poll(&pfd, 1, 200);
        if (pr <= 0) {
            /* Nothing yet — let our own source (or the sender) make progress. */
            if (pr == 0) { wl_display_roundtrip(pl->display); continue; }
            break;
        }
        char buf[4096];
        ssize_t r = read(fds[0], buf, sizeof buf);
        if (r > 0) {
            char *grown = realloc(out, total + (size_t)r);
            if (!grown) break;
            out = grown;
            memcpy(out + total, buf, (size_t)r);
            total += (size_t)r;
        } else {
            break;   /* EOF or error */
        }
    }
    close(fds[0]);

    if (out) fx_ui_paste(pl->ui, out, total);
    free(out);
}

/* ------------------------------------------------------------------ */
/*  Seat                                                               */
/* ------------------------------------------------------------------ */

static void seat_caps(void *data, struct wl_seat *seat, uint32_t caps)
{
    typio_platform *pl = data;
    bool has_ptr = caps & WL_SEAT_CAPABILITY_POINTER;
    bool has_kb  = caps & WL_SEAT_CAPABILITY_KEYBOARD;

    if (has_ptr && !pl->pointer) {
        pl->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(pl->pointer, &pointer_listener, pl);
    } else if (!has_ptr && pl->pointer) {
        wl_pointer_destroy(pl->pointer);
        pl->pointer = NULL;
    }
    if (has_kb && !pl->keyboard) {
        pl->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(pl->keyboard, &keyboard_listener, pl);
    } else if (!has_kb && pl->keyboard) {
        wl_keyboard_destroy(pl->keyboard);
        pl->keyboard = NULL;
    }

    typio_maybe_create_data_device(pl);
}
static void seat_name(void *d, struct wl_seat *s, const char *n)
{ (void)d; (void)s; (void)n; }
static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_caps, .name = seat_name,
};

/* ------------------------------------------------------------------ */
/*  wl_output — track each output's scale for HiDPI                    */
/* ------------------------------------------------------------------ */

static typio_output *find_output(typio_platform *pl, struct wl_output *wl)
{
    for (int i = 0; i < pl->n_outputs; i++)
        if (pl->outputs[i].wl == wl) return &pl->outputs[i];
    return NULL;
}

static void out_geometry(void *d, struct wl_output *o, int32_t x, int32_t y,
                         int32_t pw, int32_t ph, int32_t sub, const char *mk,
                         const char *md, int32_t tr)
{ (void)d; (void)o; (void)x; (void)y; (void)pw; (void)ph; (void)sub; (void)mk; (void)md; (void)tr; }
static void out_mode(void *d, struct wl_output *o, uint32_t f, int32_t w,
                     int32_t h, int32_t r)
{ (void)d; (void)o; (void)f; (void)w; (void)h; (void)r; }
static void out_scale(void *data, struct wl_output *o, int32_t scale)
{
    typio_platform *pl = data;
    typio_output *out = find_output(pl, o);
    if (!out) return;
    out->scale = scale;
    typio_recompute_scale(pl);
}
static void out_name(void *d, struct wl_output *o, const char *n)
{ (void)d; (void)o; (void)n; }
static void out_desc(void *d, struct wl_output *o, const char *n)
{ (void)d; (void)o; (void)n; }
static void out_done(void *d, struct wl_output *o) { (void)d; (void)o; }

static const struct wl_output_listener output_listener = {
    .geometry = out_geometry, .mode = out_mode, .done = out_done,
    .scale = out_scale, .name = out_name, .description = out_desc,
};

/* wl_surface.enter / leave — which outputs the surface is currently on. */
static void surf_enter(void *data, struct wl_surface *s, struct wl_output *o)
{
    (void)s;
    typio_platform *pl = data;
    typio_output *out = find_output(pl, o);
    if (out) { out->entered = true; typio_recompute_scale(pl); }
}
static void surf_leave(void *data, struct wl_surface *s, struct wl_output *o)
{
    (void)s;
    typio_platform *pl = data;
    typio_output *out = find_output(pl, o);
    if (out) { out->entered = false; typio_recompute_scale(pl); }
}
/* wl_compositor v6: the compositor tells us the integer buffer scale it
 * wants for this surface. This is the modern, authoritative HiDPI signal
 * (niri and others rely on it instead of wl_output.scale + surface.enter). */
static void surf_preferred_buffer_scale(void *data, struct wl_surface *s, int32_t f)
{
    (void)s;
    typio_platform *pl = data;
    if (f >= 1) pl->pending_scale = f;
}
static void surf_preferred_buffer_transform(void *d, struct wl_surface *s, uint32_t t)
{ (void)d; (void)s; (void)t; }
static const struct wl_surface_listener surface_listener = {
    .enter = surf_enter, .leave = surf_leave,
    .preferred_buffer_scale = surf_preferred_buffer_scale,
    .preferred_buffer_transform = surf_preferred_buffer_transform,
};

/* ------------------------------------------------------------------ */
/*  Registry                                                           */
/* ------------------------------------------------------------------ */

static void reg_global(void *data, struct wl_registry *reg, uint32_t name,
                       const char *iface, uint32_t version)
{
    typio_platform *pl = data;
    if (strcmp(iface, wl_compositor_interface.name) == 0) {
        /* v6 gives wl_surface.preferred_buffer_scale (HiDPI). */
        pl->compositor = wl_registry_bind(reg, name, &wl_compositor_interface,
                                          version < 6 ? version : 6);
    } else if (strcmp(iface, xdg_wm_base_interface.name) == 0) {
        pl->wm_base = wl_registry_bind(reg, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(pl->wm_base, &wm_base_listener, pl);
    } else if (strcmp(iface, wl_seat_interface.name) == 0) {
        pl->seat = wl_registry_bind(reg, name, &wl_seat_interface,
                                    version < 5 ? version : 5);
        wl_seat_add_listener(pl->seat, &seat_listener, pl);
    } else if (strcmp(iface, wl_output_interface.name) == 0) {
        if (pl->n_outputs < TYPIO_PLATFORM_MAX_OUTPUTS) {
            /* version 2 introduces .scale; ask for it if available. */
            uint32_t v = version < 2 ? version : 2;
            typio_output *out = &pl->outputs[pl->n_outputs++];
            out->wl    = wl_registry_bind(reg, name, &wl_output_interface, v);
            out->scale = 1;
            out->entered = false;
            wl_output_add_listener(out->wl, &output_listener, pl);
        }
    } else if (strcmp(iface, wl_data_device_manager_interface.name) == 0) {
        pl->data_device_mgr = wl_registry_bind(
            reg, name, &wl_data_device_manager_interface,
            version < 3 ? version : 3);
        typio_maybe_create_data_device(pl);
    } else if (strcmp(iface, zxdg_decoration_manager_v1_interface.name) == 0) {
        pl->deco_mgr = wl_registry_bind(
            reg, name, &zxdg_decoration_manager_v1_interface, 1);
    } else if (strcmp(iface, zwp_text_input_manager_v3_interface.name) == 0) {
        pl->text_input_mgr = wl_registry_bind(
            reg, name, &zwp_text_input_manager_v3_interface, 1);
    }
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name)
{ (void)d; (void)r; (void)name; }
static const struct wl_registry_listener registry_listener = {
    .global = reg_global, .global_remove = reg_remove,
};

/* ------------------------------------------------------------------ */
/*  xdg_surface / xdg_toplevel                                          */
/* ------------------------------------------------------------------ */

static void xdg_surf_configure(void *data, struct xdg_surface *s, uint32_t serial)
{
    typio_platform *pl = data;
    xdg_surface_ack_configure(s, serial);
    if (pl->pending_w > 0 && pl->pending_h > 0 &&
        (pl->pending_w != pl->width || pl->pending_h != pl->height)) {
        pl->width  = pl->pending_w;
        pl->height = pl->pending_h;
        pl->resized = true;
    }
}
static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surf_configure,
};

static void top_configure(void *data, struct xdg_toplevel *t, int32_t w,
                          int32_t h, struct wl_array *states)
{
    (void)t; (void)states;
    typio_platform *pl = data;
    pl->pending_w = w;   /* 0 means "you choose"; handled at surf.configure */
    pl->pending_h = h;
}
static void top_close(void *data, struct xdg_toplevel *t)
{
    (void)t;
    ((typio_platform *)data)->running = false;
}
static void top_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h)
{ (void)d; (void)t; (void)w; (void)h; }
static void top_wm_caps(void *d, struct xdg_toplevel *t, struct wl_array *c)
{ (void)d; (void)t; (void)c; }
static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = top_configure, .close = top_close,
    .configure_bounds = top_bounds, .wm_capabilities = top_wm_caps,
};

/* ------------------------------------------------------------------ */
/*  Build one fx_input from the accumulated state                      */
/* ------------------------------------------------------------------ */

static void drain_input(typio_platform *pl, fx_input *in, float dt)
{
    memset(in, 0, sizeof *in);
    in->cursor       = (flux_point){ (float)pl->acc.cx, (float)pl->acc.cy };
    in->display_size = (flux_point){ (float)pl->width, (float)pl->height };
    in->dt_seconds   = dt;
    in->mods         = pl->acc.mods;
    in->scroll_x     = (float)pl->acc.scroll_x;
    in->scroll_y     = (float)pl->acc.scroll_y;
    for (int i = 0; i < FX_MOUSE_COUNT; i++) {
        in->mouse_down[i]     = pl->acc.down[i];
        in->mouse_pressed[i]  = pl->acc.pressed[i];
        in->mouse_released[i] = pl->acc.released[i];
    }
    memcpy(in->text_utf8, pl->acc.text, sizeof in->text_utf8);
    memcpy(in->preedit_utf8, pl->acc.preedit, sizeof in->preedit_utf8);
    in->preedit_cursor = pl->acc.preedit_cursor;
    in->key_count = pl->acc.key_count;
    for (uint32_t i = 0; i < pl->acc.key_count; i++) in->keys[i] = pl->acc.keys[i];

    /* clear per-frame edges; keep level state (down/cursor/mods) */
    for (int i = 0; i < FX_MOUSE_COUNT; i++)
        pl->acc.pressed[i] = pl->acc.released[i] = false;
    pl->acc.scroll_x = pl->acc.scroll_y = 0.0;
    pl->acc.key_count = 0;
    pl->acc.text[0] = '\0';
}

/* ------------------------------------------------------------------ */
/*  Pump Wayland events without blocking the render loop               */
/* ------------------------------------------------------------------ */

static void pump_events(typio_platform *pl)
{
    struct wl_display *d = pl->display;
    while (wl_display_prepare_read(d) != 0)
        wl_display_dispatch_pending(d);
    wl_display_flush(d);

    struct pollfd pfd = { .fd = wl_display_get_fd(d), .events = POLLIN };
    if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN))
        wl_display_read_events(d);
    else
        wl_display_cancel_read(d);

    wl_display_dispatch_pending(d);
}

/* ------------------------------------------------------------------ */
/*  System theme detection (best-effort, no extra deps)               */
/* ------------------------------------------------------------------ */

static bool typio_system_prefers_dark(void)
{
    FILE *fp = popen("gsettings get org.gnome.desktop.interface color-scheme 2>/dev/null", "r");
    if (fp) {
        char buf[64] = {0};
        if (fgets(buf, sizeof buf, fp)) {
            pclose(fp);
            for (char *p = buf; *p; ++p) {
                if (strncmp(p, "prefer-dark", 11) == 0)
                    return true;
            }
        } else {
            pclose(fp);
        }
    }

    const char *gtk_theme = getenv("GTK_THEME");
    if (gtk_theme) {
        for (const char *p = gtk_theme; *p; ++p) {
            if (strncasecmp(p, "dark", 4) == 0)
                return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------ */
/*  Vulkan surface helpers (platform-specific)                        */
/* ------------------------------------------------------------------ */

static VkSurfaceKHR
typio_create_vk_surface(const flux_device *device,
                     struct wl_display *display,
                     struct wl_surface *wl_surface)
{
    VkWaylandSurfaceCreateInfoKHR wsci = {
        .sType   = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
        .display = display,
        .surface = wl_surface,
    };
    VkSurfaceKHR vk_surface = VK_NULL_HANDLE;
    if (vkCreateWaylandSurfaceKHR(flux_device_vk_instance(device), &wsci, NULL,
                                  &vk_surface) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return vk_surface;
}

static void
typio_destroy_vk_surface(const flux_device *device, VkSurfaceKHR vk_surface)
{
    if (vk_surface && device)
        vkDestroySurfaceKHR(flux_device_vk_instance(device), vk_surface, NULL);
}

/* ------------------------------------------------------------------ */
/*  Run                                                                */
/* ------------------------------------------------------------------ */

int typio_platform_run(const typio_platform_config *cfg)
{
    typio_platform pl = {
        .running       = true,
        .width         = cfg->width  > 0 ? cfg->width  : 960,
        .height        = cfg->height > 0 ? cfg->height : 720,
        .buffer_scale  = 1,
        .pending_scale = 1,
    };

    /* --- Wayland connection + globals ---------------------------- */
    pl.display = wl_display_connect(NULL);
    if (!pl.display) { fprintf(stderr, "no Wayland display (is a compositor running?)\n"); return 1; }
    pl.xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);

    pl.registry = wl_display_get_registry(pl.display);
    wl_registry_add_listener(pl.registry, &registry_listener, &pl);
    wl_display_roundtrip(pl.display);   /* bind globals */
    wl_display_roundtrip(pl.display);   /* seat caps -> pointer/keyboard */

    if (pl.text_input_mgr && pl.seat) {
        pl.text_input = zwp_text_input_manager_v3_get_text_input(
            pl.text_input_mgr, pl.seat);
        zwp_text_input_v3_add_listener(pl.text_input, &text_input_listener, &pl);
    }

    if (!pl.compositor || !pl.wm_base) {
        fprintf(stderr, "compositor missing wl_compositor / xdg_wm_base\n");
        return 1;
    }

    /* --- Vulkan instance via flux (Wayland WSI extensions) ------- */
    const char *inst_exts[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME,
    };
    const char *dev_exts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    flux_device_desc ddesc = {
        .type                              = FLUX_TYPE_DEVICE_DESC,
        .log                               = flux_console_logger,
        .validation                        = FLUX_VALIDATION_AUTO,
        .required_instance_extensions      = inst_exts,
        .required_instance_extension_count = sizeof inst_exts / sizeof *inst_exts,
        .required_device_extensions        = dev_exts,
        .required_device_extension_count   = sizeof dev_exts / sizeof *dev_exts,
        .frames_in_flight                  = 2,
    };
    flux_device *device = NULL;
    if (flux_device_create(&ddesc, &device) != FLUX_OK) {
        fprintf(stderr, "flux_device_create failed\n");
        return 1;
    }

    /* --- xdg-shell window --------------------------------------- */
    pl.surface     = wl_compositor_create_surface(pl.compositor);
    wl_surface_add_listener(pl.surface, &surface_listener, &pl);
    pl.xdg_surface = xdg_wm_base_get_xdg_surface(pl.wm_base, pl.surface);
    xdg_surface_add_listener(pl.xdg_surface, &xdg_surface_listener, &pl);
    pl.toplevel    = xdg_surface_get_toplevel(pl.xdg_surface);
    xdg_toplevel_add_listener(pl.toplevel, &toplevel_listener, &pl);
    xdg_toplevel_set_title(pl.toplevel, cfg->title ? cfg->title : "Typio Control");
    xdg_toplevel_set_app_id(pl.toplevel, "com.hihusky.typio.settings");

    /* Ask the compositor for a server-side title bar when it can. */
    if (pl.deco_mgr) {
        pl.deco = zxdg_decoration_manager_v1_get_toplevel_decoration(
            pl.deco_mgr, pl.toplevel);
        zxdg_toplevel_decoration_v1_set_mode(
            pl.deco, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    } else {
        fprintf(stderr, "note: compositor offers no server-side decorations "
                        "(no title bar)\n");
    }

    wl_surface_commit(pl.surface);
    wl_display_roundtrip(pl.display);   /* receive + ack initial configure */
    wl_display_roundtrip(pl.display);   /* receive .enter(output) for scale */

    /* If no output reported yet (rare), keep the default scale = 1; the
     * frame loop will pick up changes via pl.pending_scale. */
    if (pl.pending_scale > 0) pl.buffer_scale = pl.pending_scale;

    /* --- Vulkan surface + flux surface/canvas ------------------- */
    VkSurfaceKHR vk_surface = typio_create_vk_surface(device, pl.display, pl.surface);
    if (!vk_surface) {
        fprintf(stderr, "vkCreateWaylandSurfaceKHR failed\n");
        flux_device_release(device);
        return 1;
    }

    /* Swapchain is sized in *device* pixels — logical × buffer_scale —
     * while layout and fx_input stay in logical units. */
    wl_surface_set_buffer_scale(pl.surface, pl.buffer_scale);
    wl_surface_commit(pl.surface);

    flux_surface_desc sdesc = {
        .type           = FLUX_TYPE_SURFACE_DESC,
        .vk_surface_khr = vk_surface,
        .width          = (uint32_t)(pl.width  * pl.buffer_scale),
        .height         = (uint32_t)(pl.height * pl.buffer_scale),
        .vsync          = true,
    };
    flux_surface *surface = NULL;
    if (flux_surface_create(device, &sdesc, &surface) != FLUX_OK) {
        fprintf(stderr, "flux_surface_create failed\n");
        typio_destroy_vk_surface(device, vk_surface);
        flux_device_release(device);
        return 1;
    }

    flux_canvas *canvas = NULL;
    if (flux_canvas_create(&(flux_canvas_desc){
            .type = FLUX_TYPE_CANVAS_DESC, .surface = surface }, &canvas) != FLUX_OK) {
        fprintf(stderr, "flux_canvas_create failed\n");
        flux_surface_release(surface);
        typio_destroy_vk_surface(device, vk_surface);
        flux_device_release(device);
        return 1;
    }

    fx_ui *ui = NULL;
    if (fx_ui_create(&(fx_ui_desc){
            .device = device,
            .theme  = cfg->dark ? fx_theme_dark()
                                : (typio_system_prefers_dark() ? fx_theme_dark()
                                                            : fx_theme_default()),
            .scale  = (float)pl.buffer_scale,
            .clipboard = { .set_text     = clip_set_text,
                           .request_text = clip_request_text,
                           .user         = &pl } },
            &ui) != FLUX_OK) {
        fprintf(stderr, "fx_ui_create failed\n");
        flux_canvas_destroy(canvas);
        flux_surface_release(surface);
        typio_destroy_vk_surface(device, vk_surface);
        flux_device_release(device);
        return 1;
    }

    /* --- Frame loop (present vsync-paces us) --------------------- */
    struct timespec prev;
    clock_gettime(CLOCK_MONOTONIC, &prev);

    pl.ui = ui;

    while (pl.running) {
        pump_events(&pl);

        /* Scale change (e.g. surface dragged to a HiDPI output): apply
         * the new buffer scale, resize the swapchain in device pixels,
         * and tell fx_ui so its replay transform matches. */
        if (pl.pending_scale > 0 && pl.pending_scale != pl.buffer_scale) {
            pl.buffer_scale = pl.pending_scale;
            wl_surface_set_buffer_scale(pl.surface, pl.buffer_scale);
            wl_surface_commit(pl.surface);
            fx_ui_set_scale(ui, (float)pl.buffer_scale);
            pl.resized = true;
        }
        if (pl.resized) {
            (void)flux_surface_resize(
                surface,
                (uint32_t)(pl.width  * pl.buffer_scale),
                (uint32_t)(pl.height * pl.buffer_scale));
            pl.resized = false;
        }

        flux_frame *frame = NULL;
        flux_result r = flux_surface_begin_frame(surface, NULL, &frame);
        if (r == FLUX_ERROR_SURFACE_LOST) {
            (void)flux_surface_resize(
                surface,
                (uint32_t)(pl.width  * pl.buffer_scale),
                (uint32_t)(pl.height * pl.buffer_scale));
            continue;
        }
        if (r == FLUX_ERROR_INVALID_STATE) continue;
        if (r != FLUX_OK) break;

        flux_surface_info info;
        flux_surface_get_info(surface, &info);

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        float dt = (float)(now.tv_sec - prev.tv_sec)
                 + (float)(now.tv_nsec - prev.tv_nsec) * 1e-9f;
        if (dt <= 0.0f) dt = 1.0f / 60.0f;
        prev = now;

        fx_input in;
        drain_input(&pl, &in, dt);

        /* Process GLib events (TIP client I/O, timers, etc.) */
        g_main_context_iteration(NULL, FALSE);

        fx_ui_begin(ui, &in);
        if (cfg->build) cfg->build(ui, cfg->user);
        fx_ui_end(ui);

        /* Update IME cursor rectangle */
        if (pl.text_input && pl.text_input_surface) {
            flux_rect caret = fx_ui_caret_rect(ui);
            if (caret.w > 0.0f) {
                zwp_text_input_v3_set_cursor_rectangle(
                    pl.text_input,
                    (int32_t)caret.x, (int32_t)caret.y,
                    (int32_t)caret.w, (int32_t)caret.h);
                zwp_text_input_v3_commit(pl.text_input);
            }
        }

        /* Clear to the current theme's body background so empty areas
         * (e.g. short content in a tall window) don't show a hard-coded
         * dark color in light mode. */
        fx_theme th = fx_ui_theme(ui);
        flux_color clear = th.color_bg;
        if (flux_canvas_begin(canvas, frame, &clear) == FLUX_OK) {
            (void)fx_ui_render(ui, canvas);
            flux_canvas_end(canvas);
        }

        if (flux_frame_submit(frame) != FLUX_OK) break;
        r = flux_frame_present(frame);
        if (r == FLUX_ERROR_SURFACE_LOST)
            (void)flux_surface_resize(
                surface,
                (uint32_t)(pl.width  * pl.buffer_scale),
                (uint32_t)(pl.height * pl.buffer_scale));
        else if (r != FLUX_OK) break;

    }

    /* --- Teardown ----------------------------------------------- */
    flux_device_wait_idle(device);
    fx_ui_destroy(ui);
    flux_canvas_destroy(canvas);
    flux_surface_release(surface);
    typio_destroy_vk_surface(device, vk_surface);
    flux_device_release(device);

    if (pl.deco)        zxdg_toplevel_decoration_v1_destroy(pl.deco);
    if (pl.toplevel)    xdg_toplevel_destroy(pl.toplevel);
    if (pl.xdg_surface) xdg_surface_destroy(pl.xdg_surface);
    if (pl.surface)     wl_surface_destroy(pl.surface);
    for (int i = 0; i < pl.n_outputs; i++)
        if (pl.outputs[i].wl) wl_output_destroy(pl.outputs[i].wl);
    if (pl.xkb_state)   xkb_state_unref(pl.xkb_state);
    if (pl.xkb_keymap)  xkb_keymap_unref(pl.xkb_keymap);
    if (pl.xkb_ctx)     xkb_context_unref(pl.xkb_ctx);
    if (pl.pointer)     wl_pointer_destroy(pl.pointer);
    if (pl.keyboard)    wl_keyboard_destroy(pl.keyboard);
    if (pl.text_input)  zwp_text_input_v3_destroy(pl.text_input);
    wl_display_disconnect(pl.display);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Monotonic time helper                                              */
/* ------------------------------------------------------------------ */

uint64_t typio_platform_monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}
