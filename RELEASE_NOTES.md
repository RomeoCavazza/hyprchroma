Hyprchroma v3 delivers the first adaptive shader-based release of this v0.54.2 port. The plugin no longer relies on a single uniform window overlay as its primary path. It now traverses window surfaces and subsurfaces, applying a per-surface chromakey tint that preserves bright and saturated content more effectively.

## What changed in v3

- Replaced the v2-first uniform overlay model with a per-surface shader path.
- Preserved bright pixels using a luminance-based protection mask.
- Preserved vivid UI elements and artwork using a saturation-based protection mask.
- Reduced false tinting on complex apps by traversing surfaces and subsurfaces instead of treating the entire window as one texture.
- Kept a fallback overlay path for unsupported or unusable surface textures.

## Render model

```
v2
  Window
    -> one overlay box
    -> uniform tint

v3
  Window
    -> breadth-first surface traversal
    -> one shader pass per usable surface
    -> adaptive tint mask:
         dark pixels + muted pixels -> tinted
         bright pixels or saturated pixels -> preserved
```

## Diff

|                     | v2.0.0-v054              | v3.x                      |
|---------------------|--------------------------|---------------------------|
| Primary path        | Uniform overlay          | Per-surface shader pass   |
| Window treatment    | Whole window at once     | Surface/subsurface aware  |
| Pixel preservation  | Limited                  | Bright and vivid pixels preserved |
| Complex app support | Partial                  | Improved                  |
| Fallback            | Always acceptable        | Only for unsupported surfaces |

Configuration keys under `plugin:darkwindow:*` remain backward compatible, with additional tuning for bright and saturated pixel protection.

## Tested environment

- Hyprland `59f9f268` (`v0.54.2`)
- Hyprutils `0.11.0`
- Hyprlang `0.6.8`
- Aquamarine `0.10.0`
- NixOS `26.05 (Yarara)`

## Credits

- [alexhulbert/Hyprchroma](https://github.com/alexhulbert/Hyprchroma) — original plugin
- [micha4w/Hypr-DarkWindow](https://github.com/micha4w/Hypr-DarkWindow) — ancestor project
