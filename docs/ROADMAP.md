# Roadmap of the plugins

The compositor ([its roadmap](https://github.com/L0rdr-ln/Simple-Floating-Wayland-Compositor/blob/main/docs/ROADMAP.md))
only carries the plugin API; everything optional is meant to live here. This list is ideas ordered
by how well they fit the API today, not promises. Check things off when they land.

## Done
- [x] `animations`: Hyprland style rules and curves, popin/slide/slidefade, workspace slides,
      border fade, layer fade-in, fire / squeeze / zoom (ported from the compositor's former
      built-in engine)
- [x] `wobbly`: spring mesh, tiles along the mesh
- [x] CI: unit tests under ASan/UBSan, plugins built against the compositor as a subproject, the
      compositor's end-to-end scenarios run with the plugins loaded

- [x] Theme pack: the eight extra themes of the compositor, with previews and tests

## Fits the API today
- [ ] `dim-inactive`: dim windows that do not have the focus (opacity, with a fade); needs only
      the focus hook and the scene tree
- [ ] `layers-out`: animate layer surfaces when they close (a copy of the picture, like windows)
- [ ] `minimize-genie`: a minimize animation that sucks the window into a corner, drawn with the
      same tiles as wobbly (needs a hook when a window is minimized/restored, API 3)
- [ ] `ripple`: a small expanding ring where you click (an overlay buffer)
- [ ] `night-light`: tint all outputs warm in the evening (a full-screen rect with a blend, or
      the compositor's gamma-control once it exists)

## Needs more from the API (candidates for API 3)
- [ ] `window-rules`: per application geometry, workspace, opacity (needs a hook before a window
      is placed and its app id / title)
- [ ] `hot-corners` and `edge-actions` (pointer events)
- [ ] `scratchpad`: a window that toggles in and out with a key (key events, window control)
- [ ] `snap-zones`: tiling assist while dragging (pointer events, window geometry control)
- [ ] `ipc` / `sfwcctl`: a socket to list and control windows (window control)

## Not possible with the scene graph (so not planned)
- Real GPU effects: blur, window rotation (Wayfire's spin), the cube. wlroots' scene graph draws
  pictures and rectangles; a plugin cannot run shaders on them. A different renderer would be
  needed.

## Packaging
- [ ] Release tags matching compositor releases, a Debian/Arch package that depends on the
      compositor's wlroots version
