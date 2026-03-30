# Hyprchroma

> Per-window glass tint overlay for **Hyprland v0.54.2** (NixOS 26.05 Yarara)

[![Hyprland](https://img.shields.io/badge/Hyprland-v0.54.2-94e2d5?style=flat-square&logo=wayland&logoColor=white)](https://hyprland.org)
[![NixOS](https://img.shields.io/badge/NixOS-26.05_Yarara-5277C3?style=flat-square&logo=nixos&logoColor=white)](https://nixos.org)
[![License](https://img.shields.io/badge/License-MIT-green?style=flat-square)](LICENSE)

A Hyprland plugin that applies a configurable color tint overlay on all windows, perfect for creating a unified glass/translucent desktop aesthetic. Fork of [alexhulbert/Hyprchroma](https://github.com/alexhulbert/Hyprchroma), **completely rewritten** for Hyprland v0.54.2's new rendering pipeline.

## What Changed (v2.0.0)

This is a **complete rewrite** of the original shader-based approach. The upstream plugin (targeting ≤ v0.36) is incompatible with Hyprland v0.54.2 due to major API breaking changes.

| Feature | Upstream (v1.x) | This Fork (v2.0.0) |
|---------|-----------------|---------------------|
| Architecture | 6 source files, GL shader swapping | **Single file** (250 LOC) |
| Render method | Shader hooks + chroma key | `CRectPassElement` overlay |
| Event API | `registerCallbackDynamic` (deprecated) | `Event::bus()` (native v0.54.2) |
| Animation sync | ❌ None | ✅ `m_renderOffset` (workspace slide) |
| Alpha sync | ❌ None | ✅ `m_alpha` + `m_activeInactiveAlpha` |
| Ghost layer fix | ❌ | ✅ `m_visible` + render guard |
| Fullscreen option | ❌ | ✅ `enable_on_fullscreen` |

### Fixes Applied
1. **Ghost layer elimination** — Skip hidden windows and check `m_visible` on workspace
2. **Animation sync** — Overlay follows workspace slide/fade via `m_renderOffset`
3. **Alpha sync** — Tint fades with window opacity and focus transitions
4. **Render guard** — Prevents double-exposure on active windows
5. **Fullscreen control** — Optional tint disable on fullscreen windows
6. **API migration** — All hooks use `Event::bus()` (v0.54.2 native)

## Configuration

Add to your `hyprland.conf`:

```ini
# Load the plugin
plugin = /path/to/libhyprchroma.so

# Tint color (RGB, 0.0–1.0)
plugin:darkwindow:tint_r        = 0.20
plugin:darkwindow:tint_g        = 0.70
plugin:darkwindow:tint_b        = 1.00

# Tint opacity (0.0 = invisible, 1.0 = opaque)
plugin:darkwindow:tint_strength = 0.055

# Apply tint on fullscreen windows (0 = no, 1 = yes)
plugin:darkwindow:enable_on_fullscreen = 0

# Toggle tint globally
bind = SUPER, P, togglechromakey

# Toggle per-window: togglechromakey address:0x<addr>
# Or use the alias: darkwindow:shade
```

## Installation

### NixOS (Home Manager)

```nix
# In your home.nix or equivalent
hyprchroma-src = pkgs.writeText "main.cpp" (builtins.readFile ./pkgs/hyprchroma/main.cpp);
hypr-darkwindow = pkgs.stdenv.mkDerivation {
  pname   = "hypr-darkwindow";
  version = "2.0.0";
  srcs        = [];
  dontUnpack  = true;
  nativeBuildInputs = [ pkgs.pkg-config ];
  buildInputs = [ hyprland-pkg ] ++ hyprland-pkg.buildInputs;
  buildPhase = ''
    g++ -shared -fPIC -std=c++2b -O2 \
      $(pkg-config --cflags hyprland pixman-1 libdrm) \
      ${hyprchroma-src} \
      -o libhypr-darkwindow.so
  '';
  installPhase = ''
    mkdir -p $out/lib
    cp libhypr-darkwindow.so $out/lib/
  '';
};
```

### Nix Flake

```nix
{
  inputs.hyprchroma.url = "github:RomeoCavazza/Hyprchroma";
  # ...
}
```

### Manual Build

```bash
# Requires Hyprland v0.54.2 headers installed
make
# Load: hyprctl plugin load ./out/hyprchroma.so
```

## System Requirements

- **Hyprland** v0.54.2 (commit `59f9f268`)
- **Hyprutils** ≥ 0.11.0
- **Hyprlang** ≥ 0.6.8
- **Aquamarine** ≥ 0.10.0
- **NixOS** 26.05 (Yarara) — or any distro with matching Hyprland version

## Credits

- [alexhulbert/Hyprchroma](https://github.com/alexhulbert/Hyprchroma) — Original chroma key plugin
- [micha4w/Hypr-DarkWindow](https://github.com/micha4w/Hypr-DarkWindow) — Ancestor project
- v0.54.2 port by [@RomeoCavazza](https://github.com/RomeoCavazza)

## License

MIT — see [LICENSE](LICENSE)
