# Hyprchroma
Hyprchroma is a Hyprland plugin that applies a chromakey effect for global window background transparency without affecting readability

## Configuration
```conf
# hyprland.conf
windowrulev2 = plugin:chromakey,fullscreen:0
chromakey_background = rgb(07121b)
```

Also adds 2 Dispatches `togglewindowchromakey WINDOW` and `togglechromakey` (for the active window)

## Installation

For more detailed instructions, see [my dotfiles](https://github.com/alexhulbert/SeaGlass)

### Hyprpm

```sh
hyprpm add https://github.com/alexhulbert/Hyprchroma
hyprpm enable hyprchroma
hyprpm reload
```

### Nix

For nix instructions, refer to the parent repository.
