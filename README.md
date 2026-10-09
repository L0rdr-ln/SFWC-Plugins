# sfwc-plugins

Plugins for [SFWC](https://github.com/L0rdr-ln/Simple-Floating-Wayland-Compositor), a small
floating Wayland compositor. The compositor stays minimal; effects and extras live here, so
you only install what you want. How plugins work: the compositor's
[docs/PLUGINS.md](https://github.com/L0rdr-ln/Simple-Floating-Wayland-Compositor/blob/main/docs/PLUGINS.md).

| plugin | |
|---|---|
| `wobbly` | windows wobble like jelly when they move |

## Build and install

Needs sfwc installed (it installs the `sfwc-plugin` pkg-config file and the header) and the same
wlroots version it was built with.

```sh
meson setup build
meson compile -C build
meson install -C build        # to the plugin directory of sfwc
```

Then enable a plugin in `~/.config/sfwc/sfwc.conf`:

```ini
[plugins]
load = wobbly
```

Settings of each plugin are in its own section below; plugins are loaded, unloaded and
reconfigured live when the config file is saved.

## wobbly

```ini
[plugin:wobbly]
grid = 6        # mesh nodes per side, 3 to 12
spring = 120    # how hard a node is pulled back to its place, 1 to 1000
friction = 9    # damping, 0.5 to 100; about 2*sqrt(spring) is critical (no overshoot)
```

wlroots' scene graph cannot warp a picture, so while a window swings it is cut into tiles of
about 40 px that are moved and stretched along a spring mesh. Edges of tiles are straight, so large
stretches can show faint seams, and big windows cost some CPU while they swing. Windows that are
being resized or faded in are left alone.

## Testing

`meson test -C build` runs the unit tests (the spring mesh). The compositor's end-to-end test
client has a `wobbly` scenario that drags a window and checks it trails and then settles; the CI
here builds sfwc and runs it against these plugins:

```sh
SFWC_PLUGIN_PATH=<this build dir> bash sfwc/tests/run_client_test.sh sfwc/build/sfwc sfwc/build/tests/client_test wobbly
```

## License

MIT, see LICENSE.
