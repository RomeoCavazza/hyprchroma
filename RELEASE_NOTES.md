## v3.3.0-v054

Hyprchroma v3.3.0-v054 hardens the adaptive shader path for pixel-perfect use on Hyprland v0.54.2.

## Highlights

- Removed cursor-driven mask decorrelation from the fragment shaders.
- Replaced the old one-pass-per-surface path with a grouped per-window pass.
- Added stencil guarding so overlapping subsurfaces do not stack tint unexpectedly.
- Kept rounded corners only on the root surface to avoid clipping child UI elements like toolbar buttons.
- Added `plugin:darkwindow:tint_all_surfaces` to allow debugging root-only tinting if needed.

## Why this release exists

v3.2.0-v054 introduced the adaptive per-surface shader path, but difficult layouts could still expose artifacts:

- dark pages with dense controls
- half-tiled windows
- areas near top and left edges

The underlying issue was not just repaint timing. The previous pipeline also allowed the visible mask to depend on cursor position, and it rendered surfaces independently in a way that could exaggerate artifacts in complex UI regions.

v3.3 removes that cursor dependency and switches to a grouped render model that is much more stable on real applications like GitHub and other dark UIs.

## Render model

```text
v3.2
  Window
    -> breadth-first surface traversal
    -> independent pass per usable surface
    -> optional cursor-driven mask perturbation

v3.3
  Window
    -> breadth-first surface traversal
    -> grouped adaptive pass for the whole window
    -> stencil guard prevents stacked tinting
    -> no cursor-driven mask perturbation
```

## Compatibility

- Target Hyprland: `v0.54.2`
- Tested stack:
  - Hyprland `59f9f268`
  - Hyprutils `0.11.0`
  - Hyprlang `0.6.8`
  - Aquamarine `0.10.0`
  - NixOS `26.05 (Yarara)`

## Upgrade note

Existing `plugin:darkwindow:*` settings remain compatible.

If you want the most conservative path for debugging, you can temporarily set:

```conf
plugin:darkwindow:tint_all_surfaces = 0
```

The default remains `1`.
