# Hyprchroma v3.4.0-v054 Publishing Bundle

## Scope

This release unifies the recent Hyprland `v0.54.2` work into a single public version.

It includes:

- the Hyprland `v0.54.2` port
- the adaptive grouped tint pipeline
- workspace-switch smoothing via `plugin:darkwindow:suspend_on_workspace_switch_ms`
- the optional unified window composition pass
- the guarded native surface shader path for better hover stability on dense dark UIs
- README/install cleanup so the fork is documented more honestly

## Files to sync to `RomeoCavazza/Hyprchroma`

- `src/main.cpp`
- `README.md`
- `RELEASE_NOTES.md`
- `flake.nix`

Optional:

- `res/preview.png` only if you want refreshed visuals
- repo workflows only if the public repo differs from local CI

Do not sync NixOS-local files:

- `/etc/nixos/config/hypr/theme/hyprchroma.conf`
- `/etc/nixos/home/tco/home.nix`

## Suggested tag

`v3.4.0-v054`

## Suggested release title

`v3.4.0-v054 — Unified adaptive tint release for Hyprland v0.54.2`

## Suggested release body

```md
## Highlights

- Ports Hyprchroma to Hyprland `v0.54.2`
- Ships the grouped adaptive tint pipeline
- Adds `plugin:darkwindow:suspend_on_workspace_switch_ms`
- Adds the optional `plugin:darkwindow:unified_window_pass`
- Adds the guarded `plugin:darkwindow:native_surface_shader_pass`
- Improves hover stability and reduces dark cursor trails on dense dark interfaces

## Why this release exists

This release consolidates the fork's recent progress into one coherent public version for Hyprland `v0.54.2`.

The original upstream plugin targets an older rendering path. This fork keeps the same project identity, but reworks the internals for the modern Hyprland render API and a more precise adaptive tint pipeline.

The biggest recent step is the guarded native surface shader path, which moves the tint logic closer to Hyprland's own surface rendering. In practice, that reduces inter-surface seams, improves preservation of vivid UI elements, and greatly reduces cursor-induced dark trails on complex dark UIs.

## Main config knobs

```conf
plugin:darkwindow:suspend_on_workspace_switch_ms = 150
plugin:darkwindow:unified_window_pass = 1
plugin:darkwindow:native_surface_shader_pass = 1
```

Set any of them to `0` to disable the corresponding behavior.

## Compatibility

- Target Hyprland: `v0.54.2`
- Existing `plugin:darkwindow:*` settings remain compatible with the forked implementation
```

## Publish checklist

1. Sync the files above into `RomeoCavazza/Hyprchroma`
2. Commit with something like:
   `release: cut v3.4.0-v054`
3. Replace old public releases/tags:
   - delete `v2.0.0-v054`
   - delete `v3.2.0-v054`
   - delete `v3.3.0-v054`
   - delete `v3.3.1-v054`
4. Tag the unified release:
   `git tag -a v3.4.0-v054 -m "v3.4.0-v054"`
5. Push branch + tag
6. Create one GitHub release using the body above

## Local verification

Recent local verification used:

`nix build /etc/nixos#nixosConfigurations.nixos.config.system.build.toplevel --no-link -L`

Result: passes with the current Hyprchroma fork integrated into the NixOS build.
