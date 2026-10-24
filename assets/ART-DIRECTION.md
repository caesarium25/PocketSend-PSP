# PocketSend artwork

Wallpaper generated using the built-in imagegen tool (new-image mode).

Exact prompt:

> Create a production background artwork for PocketSend, a wireless file transfer app on a Sony PSP. Wide landscape 16:9 image. Premium retro-futurist handheld aesthetic: very dark midnight navy base, flowing translucent cyan and teal glass ribbons sweeping along the far right edge and bottom right corner, tiny restrained luminous particles, subtle smooth depth. Most of the left and center must be nearly empty dark navy for highly legible interface text and cards that will be drawn by code. Sophisticated calm composition, beautiful polished light, not busy. NO text, NO letters, NO logos, NO device mockup, NO borders. This is the actual wallpaper asset, full bleed.

`wallpaper-original.png` preserves the generated artwork. `wallpaper.png` is its
480 × 272 build export. `ICON0.PNG` (144 × 80) and `PIC1.PNG` (480 × 272) are
launcher exports assembled by `tools/prepare_assets.py`, using a deterministic
two-arrow mark and real font text. Runtime controls and text are drawn in C.

Noto Sans is bundled under the SIL Open Font License in `FONT-LICENSE.txt`.
Source: https://github.com/google/fonts/tree/main/ofl/notosans
Embedded raster glyphs use 15, 18 and 26 px sizes. The artwork is embedded in the
executable; no additional asset installation is required.
