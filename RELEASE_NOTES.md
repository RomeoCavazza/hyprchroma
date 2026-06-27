# Hyprchroma v0.55.4

## Highlights

- Targets Hyprland `v0.55.4`
- Replaces the old rectangle/post-window fallback with native shader injection through Hyprland's `getShaderVariant` render path
- Keeps the adaptive luminance and saturation protection logic inside the shader
- Preserves the historical `plugin:darkwindow:*` configuration keys for compatibility
- Adds `hyprctl hyprchromaprobe -j` runtime diagnostics for hook and shader-variant coverage

## Why this tag exists

Hyprland plugins are ABI-sensitive, so this repository now uses a Hyprland-aligned tag for the public build.

The `v0.55.4` tag marks the first Hyprchroma version where the tint path is fully native to Hyprland's `v0.55.4` surface shader pipeline. It avoids the old fallback rectangle approach, which could drift during workspace transitions and conflict with other render-path plugins.

## Main config knobs

```conf
plugin:darkwindow:tint_r        = 0.20
plugin:darkwindow:tint_g        = 0.70
plugin:darkwindow:tint_b        = 1.00
plugin:darkwindow:tint_strength = 0.040

plugin:darkwindow:protect_brights      = 1.00
plugin:darkwindow:protect_saturated    = 0.85
plugin:darkwindow:enable_on_fullscreen = 1
plugin:darkwindow:tint_all_surfaces    = 1
```

## Compatibility

- Target Hyprland: `v0.55.4`
- Runtime plugin version: `4.0.0-v055-native`
- Existing `plugin:darkwindow:*` settings remain compatible with this fork

## Local verification

Recent local verification used:

```sh
nix build --no-link --print-out-paths .#hyprchroma
nix build --no-link --print-out-paths /etc/nixos#nixosConfigurations.nixos.config.system.build.toplevel
hyprctl hyprchromaprobe -j
```

Result: the NixOS system build passes, the plugin loads through Home Manager, and the runtime probe reports compiled native shader variants with zero failed variants.
