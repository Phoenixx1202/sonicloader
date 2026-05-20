# Sonic Loader — custom home-screen tile PKG sources

Contents of this folder are the inputs you need to build a PS5 PKG that, once
installed, drops a **Sonic Loader** tile on the PS5 home screen. The tile
deeplinks to `http://127.0.0.1:6969/` so it lands directly on the web UI.

This folder is everything **except** the `eboot.bin` (you'll fetch that
separately — see below). PS5 PKG building requires Sony's `prospero-pub-cmd`
from the leaked PS5 SDK, which doesn't run on Linux. Build on Windows.

## What's here

```
pkg-source/
├── sonic-loader.gp4        ← project file passed to prospero-pub-cmd
├── sce_sys/
│   ├── param.json          ← title metadata (titleName, deeplinkUri, contentId…)
│   └── icon0.png           ← home-screen tile icon (205×256 — RESIZE to 512×512)
└── README.md               (this file)
```

## What you still need

### 1. `eboot.bin` (the WebKit shim ELF)

PS5 PKGs need an `eboot.bin` even when the actual launch behavior is
`deeplinkUri`. Easiest source: extract the eboot from the upstream
`ps5-payload-dev/websrv` Homebrew Launcher PKG, which is the same WebKit
shim we want.

Grab it from:
<https://raw.githubusercontent.com/ps5-payload-dev/websrv/refs/heads/master/homebrew/IV9999-FAKE00000_00-HOMEBREWLOADER01.pkg>

On Windows with Sony's tooling:

```
prospero-pub-cmd img_extract --content_id IV9999-FAKE00000_00-HOMEBREWLOADER01 ^
  IV9999-FAKE00000_00-HOMEBREWLOADER01.pkg extracted\
copy extracted\eboot.bin pkg-source\eboot.bin
```

(If you don't have prospero-pub-cmd's `img_extract`, the third-party
`PkgEditor` GUI handles PS4 CNT-format extraction; PS5 FIH-format requires
Sony's tool. Otherwise: any PS5 fakepkg eboot stub works — pull it from
any community PS5 fakepkg whose only purpose is opening a URL.)

### 2. Resize `icon0.png` to 512×512

`sce_sys/icon0.png` here is the bundled Sonic Loader brand icon at 205×256.
The PS5 home screen expects 512×512. On Windows:

```
magick convert pkg-source\sce_sys\icon0.png -resize 512x512^ -gravity center ^
  -background black -extent 512x512 pkg-source\sce_sys\icon0.png
```

(or open in Paint/Photoshop/etc., resize, save back over the file.)

## Building the PKG

With Sony's PS5 toolchain installed and `prospero-pub-cmd` in PATH:

```
cd pkg-source
prospero-pub-cmd img_create sonic-loader.gp4 IV9999-FAKE00001_00-SONICLOADER0001.pkg
```

That produces `IV9999-FAKE00001_00-SONICLOADER0001.pkg` (~18 MB). Drop it
into Sonic Loader's PKG Installer (Settings → 📦 PKG Installer → Install
any .pkg file) and it'll add a "Sonic Loader" tile to the home screen
that opens the web UI on `:6969`.

## Once you have the built PKG

Two ways to ship it to users:

1. Upload as a release asset on `git.etawen.dev/soniciso/sonicloader`
   alongside the ELFs, then point Sonic Loader's "Install Sonic Loader UI"
   button at it (one-line change to `LAUNCHER_PKG_URL` in
   `src/homebrew.c`).
2. Replace `payloads/sonic-loader-tile.pkg` (new path) and bundle it into
   the loader ELF. Slightly larger ELF, no internet needed for install.

Either is a small follow-up commit once the PKG is in hand.

## Why not build on Linux?

The Linux community PKG tools (`PkgTool.Core` / `LibOrbisPkg`) only handle
the older PS4 `CNT` magic. PS5 uses the `FIH` magic and only Sony's
`prospero-pub-cmd` from the leaked PS5 SDK builds those today. If a public
PS5 PKG generator ever appears we can move this whole flow into the Linux
build pipeline; until then, the manual Windows step is the bottleneck.
