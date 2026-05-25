test_control_config = executable('test_control_config',
    ['test_control_config.c', '../src/main.c', '../src/control_config.c',
     '../src/control_ui.c', '../src/control_window.c',
     '../src/control_widgets.c', '../src/control_models.c',
     '../src/control_bind.c', gresource],
    include_directories: include_directories('../src'),
    dependencies: [libtypio_dep, gtk4_dep],
    c_args: ['-DTYPIO_CONTROL_TEST'],
)
test('control_config', test_control_config,
    env: {'DISPLAY': '', 'WAYLAND_DISPLAY': '', 'GDK_BACKEND': ''})
