# Hyprchroma v3.4.0-v054

## Highlights

- Ports Hyprchroma to Hyprland `v0.54.2`
- Ships the grouped adaptive tint pipeline
- Adds `plugin:darkwindow:suspend_on_workspace_switch_ms`
- Adds the optional `plugin:darkwindow:unified_window_pass`
- Adds the guarded `plugin:darkwindow:native_surface_shader_pass`
- Adds lower-level render-path hardening so the native tint path activates on real-world clients such as Firefox, foot, and VS Code
- Adds cursor invalidation controls to reduce residual dark trails during hover-heavy redraws

## Why this release exists

This release consolidates the fork's recent progress into one coherent public version for Hyprland `v0.54.2`.

The original upstream plugin targets an older rendering path. This fork keeps the same project identity, but reworks the internals for the modern Hyprland render API and a more precise adaptive tint pipeline.

The biggest recent step is the guarded native surface shader path, which moves the tint logic closer to Hyprland's own surface rendering. The final hardening work also resolves real render-path coverage on Hyprland `v0.54.2`, adds runtime diagnostics, and greatly reduces cursor-induced dark trails on complex dark UIs.

## Main config knobs

```conf
plugin:darkwindow:suspend_on_workspace_switch_ms = 150
plugin:darkwindow:unified_window_pass = 0
plugin:darkwindow:native_surface_shader_pass = 1
plugin:darkwindow:cursor_invalidation_mode = 1
```

Set any of them to `0` to disable the corresponding behavior.

## Compatibility

- Target Hyprland: `v0.54.2`
- Existing `plugin:darkwindow:*` settings remain compatible with the forked implementation

## Local verification

Recent local verification used:

`nix build /etc/nixos#nixosConfigurations.nixos.config.system.build.toplevel --no-link -L`

Result: passes with the current Hyprchroma fork integrated into the NixOS build.
