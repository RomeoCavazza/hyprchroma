# Hyprchroma

> [!WARNING]
> This fork is no longer a drop-in reflection of upstream: it ports Hyprchroma to the modern Hyprland `v0.54.2` render API and replaces the old path with a more advanced adaptive tint pipeline, including grouped surface handling, an optional unified window pass, and a guarded native surface shader path.
> The README below stays close to the original upstream project for context, while the code shipped here has been validated on Hyprland `v0.54.2` under NixOS `26.05 (Yarara)`.

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
