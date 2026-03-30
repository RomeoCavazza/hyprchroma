# Hyprchroma

[![Build](https://github.com/RomeoCavazza/Hyprchroma/actions/workflows/build.yml/badge.svg)](https://github.com/RomeoCavazza/Hyprchroma/actions/workflows/build.yml)

![2024-10-18-000536_hyprshot](https://github.com/user-attachments/assets/d47d78e7-5ddd-4637-83d4-6a8a7be2e0ce)

Hyprchroma is a Hyprland plugin that applies a chromakey effect for global window background transparency without affecting readability.

> [!NOTE]
> This fork is a **v0.54.2 port** — the upstream plugin is incompatible with Hyprland ≥ v0.54 due to breaking API changes.
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
hyprchroma-src = pkgs.writeText "main.cpp" (builtins.readFile ./pkgs/hyprchroma/main.cpp);
hypr-darkwindow = pkgs.stdenv.mkDerivation {
  pname   = "hypr-darkwindow";
  version = "2.0.0";
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

This fork is a complete rewrite for Hyprland v0.54.2 compatibility. The upstream shader-based approach (6 source files, GL shader hooks) doesn't work with the refactored v0.54 render pipeline.

| What | Upstream | This fork |
|------|----------|-----------|
| Architecture | 6 files, shader swapping | Single file (250 LOC) |
| Render method | GL shader hooks | `CRectPassElement` overlay |
| Event API | `registerCallbackDynamic` | `Event::bus()` |
| Animation sync | None | Workspace slide offset |
| Ghost layers | Present | Fixed |
| Hyprland target | ≤ v0.36 | **v0.54.2** |

### Target environment
- Hyprland v0.54.2 (`59f9f268`)
- Hyprutils 0.11.0 / Hyprlang 0.6.8 / Aquamarine 0.10.0
- NixOS 26.05 (Yarara)

## Credits

- [alexhulbert/Hyprchroma](https://github.com/alexhulbert/Hyprchroma) — Original plugin
- [micha4w/Hypr-DarkWindow](https://github.com/micha4w/Hypr-DarkWindow) — Ancestor project

## License

[MIT](LICENSE)
