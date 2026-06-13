#ifndef TYPIO_PLATFORM_H
#define TYPIO_PLATFORM_H

#include <flux/ui.h>
#include <stdbool.h>
#include <stdint.h>

/* Called once per frame to build the UI. */
typedef void (*typio_build_fn)(fx_ui *ui, void *user);

typedef struct typio_platform_config {
    const char *title;
    int         width, height;
    bool        dark;
    typio_build_fn build;
    void       *user;
} typio_platform_config;

/* Open the window, run until it closes, tear everything down.
 * Returns 0 on success, non-zero on setup failure. */
int typio_platform_run(const typio_platform_config *cfg);

/* Monotonic time in milliseconds. */
uint64_t typio_platform_monotonic_ms(void);

#endif
