project('sfwc-plugins', 'c',
  version: '0.1.0',
  license: 'MIT',
  meson_version: '>=0.63.0',
  default_options: ['c_std=c11', 'warning_level=2', 'werror=false'])

add_project_arguments('-DWLR_USE_UNSTABLE', language: 'c')

libm = meson.get_compiler('c').find_library('m')

# Header and wlroots come from the installed sfwc (pkg-config: sfwc-plugin). Plugins must be
# built against the same wlroots as the compositor that loads them.
sfwc_plugin = dependency('sfwc-plugin')
wlroots = dependency('wlroots-0.18')
wayland_server = dependency('wayland-server')

# Where the compositor looks for plugins (the install directory of sfwc by default).
plugin_dir = sfwc_plugin.get_variable('plugindir', default_value: get_option('prefix') / get_option('libdir') / 'sfwc' / 'plugins')

# The wobbly mesh has no wlroots dependency: unit-tested on its own.
mesh_lib = static_library('mesh', 'wobbly/mesh.c', dependencies: libm, pic: true)

shared_module('wobbly', 'wobbly/wobbly.c',
  name_prefix: '',
  link_with: mesh_lib,
  include_directories: include_directories('wobbly'),
  dependencies: [sfwc_plugin, wlroots, wayland_server, libm],
  c_args: ['-Wno-unused-parameter'],
  install: true, install_dir: plugin_dir)

test_wobbly = executable('test_wobbly', 'tests/test_wobbly.c',
  link_with: mesh_lib, include_directories: include_directories('wobbly'), dependencies: libm)
test('wobbly-mesh', test_wobbly)
