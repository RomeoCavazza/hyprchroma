# hypr-chrome
Hyprland plugin that applies a chromakey effect for global window background transparency without affecting readability

## Configuration
```conf
# hyprland.conf
chromakey_enable = fullscreen:0
chromakey_background = rgb(07121b)
```

Also adds 2 Dispatches `togglewindowchromakey WINDOW` and `togglechromakey` (for the active window)

## Installation

### Hpyrland >= v0.36.0
We now support Nix, wooo!

You should already have a fully working home-manager setup before adding this plugin.
```nix
#flake.nix
inputs = {
    home-manager = { ... };
    hyprland = { ... };
    ...
    hypr-darkwindow = {
      url = "github:micha4w/Hypr-DarkWindow/tags/v0.36.0";
      inputs.hyprland.follows = "hyprland";
    };
};

outputs = {
  home-manager,
  hypr-darkwindow,
  ...
}: {
  ... = {
    home-manager.users.micha4w = {
      wayland.windowManager.hyprland.plugins = [
        hypr-darkwindow.packages.${pkgs.system}.Hypr-DarkWindow
      ];
    };
  };
}
```

> [!NOTE]
> In this example `inputs.hpyr-darkwindow.url` sets the tag, Make sure that tag matches your Hyprland version.


### Hpyrland >= v0.34.0
Install using `hyprpm`
```sh
hyprpm add https://github.com/micha4w/Hypr-DarkWindow
hyprpm enable Hypr-DarkWindow
hyprpm reload
```

### Hpyrland >= v0.28.0
Installable using [Hyprload](https://github.com/duckonaut/hyprload)
```toml
# hyprload.toml
plugins = [
  "micha4w/Hypr-DarkWindow",
]
```
