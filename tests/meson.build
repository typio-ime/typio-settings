# Settings panel tests
#
# The pre-ADR-0003 test (`test_settings_config.c`) was tightly bound to the
# GVariant-typed `settings_test_set_*` shims that injected D-Bus property
# fixtures into the panel. Those shims were removed when the panel ported
# to the TIP v1 UDS client (ADR-0003) — there is no longer a property
# cache to inject. The test is parked here and not built; a new test suite
# targeting the TIP client and the per-binding `config.set` path is
# tracked separately.

#test_settings_config = executable('test_settings_config',
#    ['test_settings_config.c', '../src/main.c', '../src/settings_config.c',
#     '../src/settings_ui.c', '../src/settings_widgets.c',
#     '../src/settings_models.c', '../src/settings_bind.c',
#     '../src/tip_client.c', '../src/platform.c', proto_src],
#    include_directories: include_directories('../src'),
#    dependencies: [libtypio_dep, flux_ui_dep, glib_dep, gio_dep,
#                   wl_client_dep, xkb_dep, vulkan_dep],
#    c_args: ['-DTYPIO_SETTINGS_TEST'],
#)
#test('settings_config', test_settings_config,
#    env: {'DISPLAY': '', 'WAYLAND_DISPLAY': ''})
