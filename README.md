# Hyprchroma

[![CI](https://github.com/RomeoCavazza/hyprchroma/actions/workflows/ci.yml/badge.svg)](https://github.com/RomeoCavazza/hyprchroma/actions/workflows/ci.yml)
[![Build](https://github.com/RomeoCavazza/hyprchroma/actions/workflows/build.yml/badge.svg)](https://github.com/RomeoCavazza/hyprchroma/actions/workflows/build.yml)
[![Release](https://github.com/RomeoCavazza/hyprchroma/actions/workflows/release.yml/badge.svg)](https://github.com/RomeoCavazza/hyprchroma/actions/workflows/release.yml)
[![Hyprland](https://img.shields.io/badge/Hyprland-v0.55.4-58E1FF)](https://github.com/hyprwm/Hyprland/releases/tag/v0.55.4)

![2024-10-18-000536_hyprshot](https://github.com/user-attachments/assets/d47d78e7-5ddd-4637-83d4-6a8a7be2e0ce)

Hyprchroma is a Hyprland plugin that applies a chromakey effect for global window background transparency without affecting readability.

`main` and the `v0.55.4` tag target Hyprland `v0.55.4`. Hyprchroma uses native surface shader injection through Hyprland's `getShaderVariant` render path, with adaptive luminance and saturation protection in the shader. Hyprland plugins are ABI-sensitive; pin the matching Hyprland tag or a known-good commit.

## Configuration

Configuration still uses the historical `plugin:darkwindow:*` keys for compatibility.

```conf
# hyprland.conf
plugin = /path/to/libhyprchroma.so
```

```conf
# tint color
plugin:darkwindow:tint_r        = 0.20
plugin:darkwindow:tint_g        = 0.70
plugin:darkwindow:tint_b        = 1.00
plugin:darkwindow:tint_strength = 0.040

# protection masks
plugin:darkwindow:protect_brights      = 1.00
plugin:darkwindow:bright_threshold     = 0.82
plugin:darkwindow:bright_knee          = 0.12
plugin:darkwindow:protect_saturated    = 0.85
plugin:darkwindow:saturation_threshold = 0.20
plugin:darkwindow:saturation_knee      = 0.16

# behavior
plugin:darkwindow:enable_on_fullscreen = 1
plugin:darkwindow:tint_all_surfaces    = 1
```

Also adds 2 Dispatches `togglewindowchromakey WINDOW` and `togglechromakey` (for the active window).

Runtime probe:

```sh
hyprctl hyprchromaprobe -j
```

## Installation

### Nix

```nix
inputs.hyprchroma.url = "github:RomeoCavazza/hyprchroma";

wayland.windowManager.hyprland.plugins = [
  inputs.hyprchroma.packages.${pkgs.system}.hyprchroma
];
```

Hyprland plugins are ABI-sensitive. Build Hyprchroma with headers matching the Hyprland version that loads it.

### Hyprpm

Install using `hyprpm`.

```sh
hyprpm add https://github.com/RomeoCavazza/hyprchroma
hyprpm enable hyprchroma
hyprpm reload
```

### Manual build

```sh
make all
hyprctl plugin load ./out/hyprchroma.so
```
