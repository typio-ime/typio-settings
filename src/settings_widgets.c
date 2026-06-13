#include "settings_widgets.h"

#include <glib.h>

static char *settings_slugify(const char *text) {
    GString *slug;
    gboolean last_dash = FALSE;

    if (!text || !*text) {
        return g_strdup("unnamed");
    }

    slug = g_string_new(NULL);
    for (const char *p = text; *p; ++p) {
        if (g_ascii_isalnum(*p)) {
            g_string_append_c(slug, g_ascii_tolower(*p));
            last_dash = FALSE;
        } else if (!last_dash && slug->len > 0) {
            g_string_append_c(slug, '-');
            last_dash = TRUE;
        }
    }

    while (slug->len > 0 && slug->str[slug->len - 1] == '-') {
        g_string_truncate(slug, slug->len - 1);
    }

    if (slug->len == 0) {
        g_string_assign(slug, "unnamed");
    }

    return g_string_free(slug, FALSE);
}

char *settings_build_debug_name(const char *prefix, const char *token) {
    char *slug = settings_slugify(token);
    char *name;

    if (prefix && *prefix) {
        name = g_strdup_printf("%s-%s", prefix, slug);
    } else {
        name = g_strdup(slug);
    }

    g_free(slug);
    return name;
}
