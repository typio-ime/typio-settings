# ── flux-ui Settings Panel ────────────────────────────────────────────────

# Generate Wayland protocol glue for native platform layer.
wl = import('wayland')
proto_src = wl.scan_xml(
    wl.find_protocol('xdg-shell'),
    wl.find_protocol('xdg-decoration', state : 'unstable', version : 1),
    wl.find_protocol('text-input', state : 'unstable', version : 3),
)

settings_sources = files(
    'main.c',
    'settings_config.c',
    'settings_models.c',
    'settings_widgets.c',
    'settings_ui.c',
    'settings_bind.c',
    'tip_client.c',
    'platform.c',
)

settings_exe = executable('typio-settings',
    settings_sources, proto_src,
    dependencies: [libtypio_dep, flux_ui_dep, glib_dep, gio_dep, wl_client_dep, xkb_dep, vulkan_dep],
    include_directories: include_directories('.'),
    install: true,
    install_dir: get_option('bindir'),
)
