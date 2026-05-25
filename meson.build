project('typio-control', 'c',
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
gtk4_dep = dependency('gtk4', required: true)
glib_compile_resources = find_program('glib-compile-resources', required: true)

datadir = get_option('datadir')

subdir('src')

if get_option('build_tests')
    subdir('tests')
endif

# ── Desktop entry ──────────────────────────────────────────────────────────
install_data('data/applications/typio-control.desktop',
    install_dir: datadir / 'applications')
