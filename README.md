# sfwc-plugins

Plugins for [SFWC](https://github.com/L0rdr-ln/Simple-Floating-Wayland-Compositor), a small
floating Wayland compositor. The compositor stays minimal; effects and extras live here, so you
install only what you want. How plugins work (config, API, writing your own):
[docs/PLUGINS.md](https://github.com/L0rdr-ln/Simple-Floating-Wayland-Compositor/blob/main/docs/PLUGINS.md)
in the compositor repository. What may come next: [docs/ROADMAP.md](docs/ROADMAP.md).

| plugin | what it does |
|---|---|
| [`animations`](#animations) | open/close/move animations, workspace slides, border fade, layer surfaces; Hyprland style rules and curves; fire, squeeze and zoom effects |
| [`wobbly`](#wobbly) | windows wobble like jelly when they move |

## Install

Needs sfwc installed (it installs the `sfwc-plugin` pkg-config file and the header) and the same
wlroots version it was built with (0.20). Plugins use plugin API version 2, which is in
sfwc releases after 0.6.0.

```sh
meson setup build
meson compile -C build
meson install -C build        # to the plugin directory of sfwc
```

Then enable what you want in `~/.config/sfwc/sfwc.conf`; plugins are loaded, unloaded and
reconfigured live when the file is saved:

```ini
[plugins]
load = animations
load = wobbly
```

Tip: the two work together. Animations leave a window alone while it wobbles, and wobbly waits
while a window is fading or growing in.

## animations

Load it with `load = animations` in `[plugins]`. The `[animations]` section takes the same kind of lines as Hyprland, so an animation block from a
Hyprland config mostly works as it is:

```ini
[animations]
enabled = true
# a named curve: bezier = name, x0, y0, x1, y1   (x between 0 and 1, y may overshoot)
bezier = myBezier, 0.05, 0.9, 0.1, 1.05
# animation = type, on, speed, curve[, style]     speed is in units of 100 ms
animation = windows, 1, 4, myBezier
animation = windowsOut, 1, 5, default, popin 80%
animation = fade, 1, 7, default
animation = border, 1, 10, default
animation = workspaces, 1, 6, default, slide
```

Or start from a ready-made set and change what you like (later lines win):

```ini
[animations]
preset = hyprland     # hyprland | minimal | none
animation = workspaces, 1, 3, easeOutQuint, slidefade 15%
```

| Type | What moves | Styles |
|---|---|---|
| `windowsIn` / `windowsOut` / `windows` | a window opening / closing (`windows` sets both and `windowsMove`) | `popin [N%]` (grows from / shrinks to N% of its size around its center, default 80), `slide [left\|right\|top\|bottom]` (from / to the nearest screen edge, default), `slidefade [N%]` (moves by N% of its size, default 20, and fades), and the Wayfire style effects `fire`, `squeeze` and `zoom [N%]` (below) |
| `windowsMove` | a window moved by maximize, restore or to the next output | none |
| `fadeIn` / `fadeOut` / `fade` | the opacity of an opening / closing window, on its own timeline (so a window can `popin` over 0.4 s and fade over 0.2 s) | none |
| `border` | the frame colors changing when the window gains or loses focus | none |
| `workspaces` | switching workspace: the old windows leave, the new ones arrive | `slide [N%]`, `slidevert`, `slidefade [N%]`, `slidefadevert`, `fade` |
| `layers` / `layersIn` | bars, launchers and notifications appearing | `fade` (default), `popin [N%]`; `slide` is shown as a fade |
| `global` | the default for every type without its own rule (no style) | none |

- `animation = type, 0` turns one type off. The speed is in units of 100 ms, as in Hyprland, so
  `4` is 400 ms.
- Curves: `linear`, `default`, `ease`, `ease-in`, `ease-out` and `ease-in-out` are built in; define
  your own with `bezier =` **before** the lines that use them. A curve with y values above 1
  overshoots (a little bounce).
- Going to a higher workspace moves the content to the left (the new windows come in from the
  right), going back moves it the other way.
- Without any `animation =` line the simple keys (`open`, `close`, `move`, `duration_ms`, `easing`)
  (a fade with a short slide) apply. `enabled = false` and `SFWC_NO_ANIMATIONS=1` switch everything
  off.
- Not supported (they only produce a warning, so a pasted Hyprland block still loads):
  `borderangle`, `fadeSwitch`, `fadeShadow`, `fadeDim`, `fadeLayers`, `specialWorkspace`,
  `workspacesIn/Out`, `monitorAdded`, and the `gnomed` style. Layer surfaces do not animate when
  they close yet.
- Scaling windows (`popin`) is done by resizing each of the window's buffers around its center
  for the length of the animation, which is cheap but means a window that is being resized by its
  client at the same moment can flicker.

### Wayfire style effects: fire, squeeze, zoom

Three more styles for `windowsIn` / `windowsOut` (and `windows`):

```ini
[animations]
animation = windowsOut, 1, 6, default, fire      # the window burns away from the bottom
animation = windowsIn,  1, 6, default, fire      # ... and un-burns when it opens
# animation = windowsOut, 1, 4, ease, squeeze    # collapses to a line, then to nothing (TV off)
# animation = windowsIn,  1, 4, ease, zoom 70%   # grows from 70% of its size while fading in
fire_particles = 400      # at most this many flames at a time (20-2000)
fire_size = 14            # radius of a flame in px (4-60)
fire_color = #ff7a18      # the main color of the flames; the core is white-yellow, the tail red and smoke
```

- **fire**: the window's buffers are cropped along a burn line that climbs from the bottom while
  flames (a small particle simulation, drawn in software at half resolution) rise from the line.
  The burn takes the first 80% of the time, the last 20% is the flames dying down. Opening plays
  it backwards. The flames are translucent, so you can see the window burn.
- **squeeze**: the height collapses to a line first, then the width, like a switched off CRT.
- **zoom**: a popin that always fades (`popin` leaves the fading to the `fade` rule).
- Not possible with the scene graph, so not offered: Wayfire's `spin` (rotation) and the
  GPU-shader effects.

## wobbly

```ini
[plugin:wobbly]
grid = 6        # mesh nodes per side, 3 to 12
spring = 120    # how hard a node is pulled back to its place, 1 to 1000
friction = 9    # damping, 0.5 to 100; about 2*sqrt(spring) is critical (no overshoot)
```

Windows are drawn on a grid of springs. When a window moves (dragging, maximize, snapping, "next
output") the grid lags behind and swings back; while it is dragged the part near the pointer
follows closely and the far side trails.

wlroots' scene graph cannot warp a picture, so while a window swings it is cut into tiles of
about 40 px that are moved and stretched along the mesh. Edges of tiles are straight, so large
stretches can show faint seams, and big windows cost some CPU while they swing. Windows that are
being resized or faded in are left alone.

## Building against a compositor checkout

For development the compositor can be a meson subproject, so the plugins are built against
exactly its wlroots:

```sh
git clone https://github.com/L0rdr-ln/Simple-Floating-Wayland-Compositor subprojects/sfwc
meson setup build -De2e=true
meson compile -C build
meson test -C build --suite sfwc-plugins
```

`-De2e=true` also runs the compositor's end-to-end scenarios (`wobbly`, `anim`, `hypr`, `fx`: a
headless sfwc, a real Wayland client, virtual input and screen captures compared pixel by pixel)
with the plugins loaded from the build directory. `-Dmodules=false` builds only the unit tests,
which need no compositor: the spring mesh, the animation curves and rules (with line numbers in
error messages) and the flames.

## Writing a plugin

Start from `wobbly/wobbly.c` (about 450 lines, one effect) or the small test plugin in the
compositor repository (`plugins/hooktest`), and read the API description in its docs/PLUGINS.md.

## License

MIT, see LICENSE.
