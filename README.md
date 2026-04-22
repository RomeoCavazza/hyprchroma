# Hyprchroma

> [!NOTE]
> This fork keeps the upstream README below mostly unchanged for reference.
> The current fork code targets Hyprland `0.54.2` (`59f9f2688ac508a0584d1462151195a6c4992f99`) and was validated on NixOS `26.05 (Yarara)`.

![2024-10-18-000536_hyprshot](https://github.com/user-attachments/assets/d47d78e7-5ddd-4637-83d4-6a8a7be2e0ce)

Hyprchroma is a Hyprland plugin that applies a chromakey effect for global window background transparency without affecting readability

## Configuration
```conf
# hyprland.conf
windowrulev2 = plugin:chromakey,fullscreen:0
chromakey_background = 7,8,17
```

Also adds 2 Dispatches `togglewindowchromakey WINDOW` and `togglechromakey` (for the active window)

## Installation

### Hyprland >= v0.36.0
We now support Nix, wooo!

### Hyprpm

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
> In this example `inputs.hypr-darkwindow.url` sets the tag, Make sure that tag matches your Hyprland version.


### Hyprland >= v0.34.0
Install using `hyprpm`
```sh
hyprpm add https://github.com/alexhulbert/Hyprchroma
hyprpm enable hyprchroma
hyprpm reload
```

### Nix

For nix instructions, refer to the parent repository.
