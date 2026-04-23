# Hyprchroma

> [!IMPORTANT]
> This fork ports Hyprchroma to Hyprland `v0.54.2` and replaces the old implementation with a more advanced adaptive tint pipeline, including grouped surface handling, an optional unified window pass, and a guarded native surface shader path.
> The current fork has been validated on Hyprland `v0.54.2` under NixOS `26.05 (Yarara)`.

![2024-10-18-000536_hyprshot](https://github.com/user-attachments/assets/d47d78e7-5ddd-4637-83d4-6a8a7be2e0ce)

Hyprchroma is a Hyprland plugin that applies a chromakey effect for global window background transparency without affecting readability

## Configuration

This fork is configured through `plugin:darkwindow:*` keywords. A practical setup is to keep them in a dedicated `hyprchroma.conf` file and source it from `hyprland.conf`.

```conf
# hyprland.conf
plugin = $HOME/.local/lib/libhypr-darkwindow.so
source = ~/.config/hypr/theme/hyprchroma.conf
```

```conf
# ~/.config/hypr/theme/hyprchroma.conf
plugin:darkwindow:tint_r        = 0.20
plugin:darkwindow:tint_g        = 0.70
plugin:darkwindow:tint_b        = 1.00
plugin:darkwindow:tint_strength = 0.058

plugin:darkwindow:unified_window_pass = 0
plugin:darkwindow:native_surface_shader_pass = 1
plugin:darkwindow:suspend_on_workspace_switch_ms = 150
```

Useful optional knobs:

- `plugin:darkwindow:cursor_invalidation_mode`
- `plugin:darkwindow:cursor_invalidation_throttle_ms`
- `plugin:darkwindow:cursor_invalidation_radius`

Also adds 2 Dispatches `togglewindowchromakey WINDOW` and `togglechromakey` (for the active window).

## Installation

### Hyprland v0.54.2
This fork currently targets Hyprland `v0.54.2`.

### Nix
```nix
outputs = {
  home-manager,
  hyprchroma,
  ...
}: {
  ... = {
    home-manager.users.micha4w = {
      wayland.windowManager.hyprland.plugins = [
        hyprchroma.packages.${pkgs.system}.hyprchroma
      ];
    };
  };
}
```

### Hyprpm
Install using `hyprpm`
```sh
hyprpm add https://github.com/RomeoCavazza/Hyprchroma
hyprpm enable hyprchroma
hyprpm reload
```

### Manual build
```sh
make all
hyprctl plugin load ./out/hyprchroma.so
```
