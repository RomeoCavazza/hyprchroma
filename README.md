# Hyprchroma

[![Build](https://github.com/RomeoCavazza/Hyprchroma/actions/workflows/build.yml/badge.svg)](https://github.com/RomeoCavazza/Hyprchroma/actions/workflows/build.yml)
[![Release](https://img.shields.io/github/v/release/RomeoCavazza/Hyprchroma?display_name=tag)](https://github.com/RomeoCavazza/Hyprchroma/releases)

![2024-10-18-000536_hyprshot](https://github.com/user-attachments/assets/d47d78e7-5ddd-4637-83d4-6a8a7be2e0ce)

Hyprchroma is a Hyprland plugin that applies an adaptive chromakey tint for global window background transparency while preserving readability and vivid UI elements.

> [!NOTE]
> This fork is the maintained Hyprland `v0.54.2` continuation of `alexhulbert/Hyprchroma`, itself forked from `micha4w/Hypr-DarkWindow`.
> Upstream is no longer maintained for modern Hyprland.

## Configuration
```conf
# hyprland.conf
plugin:darkwindow:tint_r        = 0.20
plugin:darkwindow:tint_g        = 0.70
plugin:darkwindow:tint_b        = 1.00
plugin:darkwindow:tint_strength = 0.058
plugin:darkwindow:protect_brights      = 1.00
plugin:darkwindow:bright_threshold     = 0.55
plugin:darkwindow:bright_knee          = 0.35
plugin:darkwindow:protect_saturated    = 1.00
plugin:darkwindow:saturation_threshold = 0.05
plugin:darkwindow:saturation_knee      = 0.25
plugin:darkwindow:enable_on_fullscreen = 0
plugin:darkwindow:tint_all_surfaces    = 1
```

Also adds 2 dispatchers: `togglechromakey` and `darkwindow:shade`

Examples:
```sh
# Toggle the global tint
hyprctl dispatch togglechromakey

# Toggle a specific window override
hyprctl dispatch darkwindow:shade address:0x1234567890
```

## Installation

### Hyprland >= v0.54.2

### Hyprpm
```sh
hyprpm add https://github.com/RomeoCavazza/Hyprchroma
hyprpm enable hyprchroma
hyprpm reload
```

### Nix Flake
```nix
{
  inputs.hyprchroma.url = "github:RomeoCavazza/Hyprchroma";
  # ...
}
```

> [!NOTE]
> If you pin a tag in Nix, make sure it matches your Hyprland target version.

### Home Manager (inline build)
```nix
hyprchroma-src = pkgs.writeText "main.cpp" (builtins.readFile ./pkgs/Hyprchroma-fork/src/main.cpp);
hypr-darkwindow = pkgs.stdenv.mkDerivation {
  pname   = "hypr-darkwindow";
  version = "3.3.0-v054";
  dontUnpack = true;
  nativeBuildInputs = [ pkgs.pkg-config ];
  buildInputs = [ hyprland-pkg ] ++ hyprland-pkg.buildInputs;
  buildPhase = ''
    g++ -shared -fPIC -std=c++2b -O2 \
      $(pkg-config --cflags hyprland pixman-1 libdrm) \
      ${hyprchroma-src} -o libhypr-darkwindow.so
  '';
  installPhase = ''
    mkdir -p $out/lib
    cp libhypr-darkwindow.so $out/lib/
  '';
};
```

### Manual Build
```sh
make
hyprctl plugin load ./out/hyprchroma.so
```

## Changes from upstream

This fork started as a Hyprland `v0.54.2` compatibility rewrite. It now ships a grouped adaptive shader path that preserves bright and saturated pixels much better than a uniform overlay while remaining stable on complex dark interfaces like GitHub.

| What | Upstream | v2.0.0-v054 | v3.2.0-v054 | v3.3.0-v054 |
|------|----------|-------------|-------------|-------------|
| Architecture | 6 files, shader swapping | Compatibility rewrite | Adaptive per-surface passes | Grouped adaptive window pass |
| Render method | GL shader hooks | `CRectPassElement` uniform overlay | Independent surface shader passes | Grouped shader pass + stencil guard |
| Event API | `registerCallbackDynamic` | `Event::bus()` | `Event::bus()` | `Event::bus()` |
| Pixel preservation | Original chromakey intent | Limited | Bright/saturated pixels preserved | Same, but without cursor-driven perturbation |
| Surface handling | Legacy renderer assumptions | Whole-window overlay | Surface/subsurface traversal | Surface traversal with grouped composition |
| Hyprland target | ≤ v0.36 | v0.54.2 | v0.54.2 | **v0.54.2** |

## Target environment
- Hyprland v0.54.2 (`59f9f268`)
- Hyprutils 0.11.0 / Hyprlang 0.6.8 / Aquamarine 0.10.0
- NixOS 26.05 (Yarara)

## Credits

- [alexhulbert/Hyprchroma](https://github.com/alexhulbert/Hyprchroma) — Original plugin
- [micha4w/Hypr-DarkWindow](https://github.com/micha4w/Hypr-DarkWindow) — Ancestor project

## License

[MIT](LICENSE)
