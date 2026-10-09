project('sfwc-plugins', 'c',
  version: '0.1.0',
  license: 'MIT',
  meson_version: '>=0.63.0',
  default_options: ['c_std=c11', 'warning_level=2', 'werror=false'])

add_project_arguments('-DWLR_USE_UNSTABLE', language: 'c')

libm = meson.get_compiler('c').find_library('m')
cairo = dependency('cairo')

# ---------------------------------------------------------------- pure code, unit-tested alone
mesh_lib = static_library('mesh', 'wobbly/mesh.c', dependencies: libm, pic: true)
anim_lib = static_library('anim', 'animations/anim.c', dependencies: libm, pic: true)
animcfg_lib = static_library('animcfg', 'animations/animcfg.c', link_with: anim_lib,
  include_directories: include_directories('animations'), dependencies: libm, pic: true)
fire_lib = static_library('fire', 'animations/fire.c',
  include_directories: include_directories('animations'), dependencies: [cairo, libm], pic: true)

test('wobbly-mesh', executable('test_wobbly', 'tests/test_wobbly.c', link_with: mesh_lib,
  include_directories: include_directories('wobbly'), dependencies: libm), suite: 'sfwc-plugins')
test('animation-math', executable('test_anim', 'tests/test_anim.c', link_with: anim_lib,
  include_directories: include_directories('animations'), dependencies: libm), suite: 'sfwc-plugins')
test('animation-config', executable('test_animcfg', 'tests/test_animcfg.c', link_with: [animcfg_lib, anim_lib],
  include_directories: include_directories('animations'), dependencies: libm), suite: 'sfwc-plugins')
test('fire-simulation', executable('test_fire', 'tests/test_fire.c', link_with: fire_lib,
  include_directories: include_directories('animations'), dependencies: [cairo, libm]), suite: 'sfwc-plugins')

# ---------------------------------------------------------------- the plugins
if get_option('modules')
  # The header and wlroots come from the installed sfwc (pkg-config: sfwc-plugin) or, when there is
  # none, from sfwc as a subproject (subprojects/sfwc). Plugins must be built against the same
  # wlroots as the compositor that loads them.
  sfwc_plugin = dependency('sfwc-plugin', fallback: ['sfwc', 'sfwc_plugin_dep'])
  plugin_dir = get_option('prefix') / get_option('libdir') / 'sfwc' / 'plugins'
  plugin_args = ['-Wno-unused-parameter']

  shared_module('wobbly', 'wobbly/wobbly.c',
    name_prefix: '',
    link_with: mesh_lib,
    include_directories: include_directories('wobbly'),
    dependencies: [sfwc_plugin, libm],
    c_args: plugin_args,
    install: true, install_dir: plugin_dir)

  shared_module('animations', 'animations/animations.c',
    name_prefix: '',
    link_with: [animcfg_lib, anim_lib, fire_lib],
    include_directories: include_directories('animations'),
    dependencies: [sfwc_plugin, cairo, libm],
    c_args: plugin_args,
    install: true, install_dir: plugin_dir)

  # sfwc's end-to-end scenarios (a headless compositor and a Wayland test client) with the
  # plugins loaded from this build directory.
  if get_option('e2e')
    sfwc = subproject('sfwc')
    run_script = meson.project_source_root() / 'subprojects/sfwc/tests/run_client_test.sh'
    foreach mode : ['wobbly', 'anim', 'hypr', 'fx']
      test('compositor-client-' + mode, find_program('bash'),
        args: [run_script, sfwc.get_variable('sfwc_exe'), sfwc.get_variable('client_test'), mode],
        env: {'SFWC_PLUGIN_PATH': meson.current_build_dir()},
        suite: 'sfwc-plugins', timeout: 120,
        is_parallel: false) # the pixel checks are timing sensitive: do not let scenarios compete for CPU
    endforeach
  endif
endif
