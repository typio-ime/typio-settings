project('typio-settings', 'c',
    version: '4.2.2',
    default_options: [
        'c_std=c23',
        'warning_level=2',
        'werror=false',
    ],
    meson_version: '>=1.0.0',
)

cc = meson.get_compiler('c')
add_project_arguments('-D_POSIX_C_SOURCE=200809L', language: 'c')

# ── Dependencies ──────────────────────────────────────────────────────────
libtypio_dep = dependency('libtypio')
flux_ui_dep  = dependency('flux-ui', required: true)
glib_dep     = dependency('glib-2.0', required: true)
gio_dep      = dependency('gio-2.0',  required: true)

# Platform dependencies for native Wayland backend
wl_client_dep = dependency('wayland-client', required: true)
xkb_dep       = dependency('xkbcommon',      required: true)
vulkan_dep    = dependency('vulkan',         required: true)

datadir = get_option('datadir')

subdir('src')

if get_option('build_tests')
    subdir('tests')
endif

# ── Desktop entry ──────────────────────────────────────────────────────────
install_data('data/applications/typio-settings.desktop',
    install_dir: datadir / 'applications')
