# Launcher art: sources

Both images come from the project author's own website, https://akirahayabusa.github.io/ngb-hurricane-patch/
(fetched 2026-10-22), where they are embedded in the page's stylesheet as data URIs. The author chose them for the
launcher (2026-10-22).

| File | Size | Where on the site | SHA-256 (first 20 hex) |
|---|---|---|---|
| `hero.jpg` | 1024x512 | `.hero .art` background (Ryu Hayabusa, Ninja Gaiden key art) | `f6b255d6bf8c0ac8ff83` |
| `texture.jpg` | 512x400 | `body::before` background (dark wood with wave and crane patterns) | `537bd25e54a4797f0308` |

The Ryu Hayabusa artwork is Koei Tecmo's. It is used here as the interface of a non-commercial fan port, at the
author's decision (2026-10-22); see the legal note in README.md.

## `icon.ico`: the programs' icon

The Dragon Sword from the game's pause menu (weapons page): its texture dumped from the user's own game with the
loader's `trace`, made into an .ico (sizes 16 to 256) for the local builds on 2026-10-22, then put into the repository
and the release at the author's decision the same day ("仍用龙剑图标"). SHA-256 (first 20 hex): `eb6e2b63165e62171b14`.
Both programs carry it through PORT_ICON (loader/CMakeLists.txt). The artwork is Koei Tecmo's.
