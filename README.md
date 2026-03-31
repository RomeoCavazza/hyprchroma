# Hyprchroma

[![Build](https://github.com/RomeoCavazza/Hyprchroma/actions/workflows/build.yml/badge.svg)](https://github.com/RomeoCavazza/Hyprchroma/actions/workflows/build.yml)
[![Release](https://img.shields.io/github/v/release/RomeoCavazza/Hyprchroma?display_name=tag)](https://github.com/RomeoCavazza/Hyprchroma/releases)

Adaptive per-surface chromakey tint for Hyprland v0.54.2.

Current release: `v3.2.0-v054`

![2024-10-18-000536_hyprshot](https://github.com/user-attachments/assets/d47d78e7-5ddd-4637-83d4-6a8a7be2e0ce)

Hyprchroma is a Hyprland plugin that applies an adaptive chromakey tint while preserving readability and high-chroma UI elements.

> [!NOTE]
> This fork is a **v0.54.2 port and continuation** — the upstream plugin is incompatible with Hyprland ≥ v0.54 due to breaking API changes.
> See [Changes from upstream](#changes-from-upstream) for details.

## Configuration
```conf
# hyprland.conf

# Tint color (RGB, 0.0–1.0)
plugin:darkwindow:tint_r        = 0.20
plugin:darkwindow:tint_g        = 0.70
plugin:darkwindow:tint_b        = 1.00

# Tint opacity (0.0 = invisible, 1.0 = opaque)
plugin:darkwindow:tint_strength = 0.055

# Preserve bright and saturated pixels
plugin:darkwindow:protect_brights      = 1.00
plugin:darkwindow:bright_threshold     = 0.55
plugin:darkwindow:bright_knee          = 0.35
plugin:darkwindow:protect_saturated    = 1.00
plugin:darkwindow:saturation_threshold = 0.05
plugin:darkwindow:saturation_knee      = 0.25

# Apply tint on fullscreen windows (0 = no, 1 = yes)
plugin:darkwindow:enable_on_fullscreen = 0
```

Also adds 2 dispatchers: `togglechromakey` (for the active window) and `darkwindow:shade address:0x<addr>` (per-window toggle)

## Installation

### Hyprland v0.54.2+ (NixOS)

#### Nix Flake
```nix
{
  inputs.hyprchroma.url = "github:RomeoCavazza/Hyprchroma";
  # ...
}
```

#### Home Manager (inline build)
```nix
hyprchroma-src = pkgs.writeText "main.cpp" (builtins.readFile ./pkgs/Hyprchroma-fork/src/main.cpp);
hypr-darkwindow = pkgs.stdenv.mkDerivation {
  pname   = "hypr-darkwindow";
  version = "3.2-v054";
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

### Hyprpm
```sh
hyprpm add https://github.com/RomeoCavazza/Hyprchroma
hyprpm enable hyprchroma
hyprpm reload
```

### Manual Build
```sh
# Requires Hyprland v0.54.2 headers
make
hyprctl plugin load ./out/hyprchroma.so
```

## Changes from upstream

This fork started as a compatibility rewrite for Hyprland v0.54.2. It now ships an adaptive per-surface shader path that preserves bright and high-chroma pixels significantly better than a uniform overlay.

| What | Upstream | This fork |
|------|----------|-----------|
| Architecture | 6 files, shader swapping | Single-file plugin core with per-surface passes |
| Render method | GL shader hooks | Adaptive shader pass + fallback overlay |
| Event API | `registerCallbackDynamic` | `Event::bus()` |
| Pixel preservation | Original chromakey intent | Bright/saturated pixels preserved |
| Surface handling | Legacy renderer assumptions | Surface/subsurface traversal |
| Hyprland target | ≤ v0.36 | **v0.54.2** |

### Target environment
- Hyprland v0.54.2 (`59f9f268`)
- Hyprutils 0.11.0 / Hyprlang 0.6.8 / Aquamarine 0.10.0
- NixOS 26.05 (Yarara)

## Credits

- [alexhulbert/Hyprchroma](https://github.com/alexhulbert/Hyprchroma) — Original plugin
- [micha4w/Hypr-DarkWindow](https://github.com/micha4w/Hypr-DarkWindow) — Ancestor project

## Status

`/etc/nixos/home/tco/pkgs/Hyprchroma-fork` is the source of truth for local development.
`/etc/nixos/home/tco/pkgs/hyprchroma` can be removed once any local references to it are migrated.

## License

[MIT](LICENSE)
