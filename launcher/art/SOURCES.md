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

## `hero_ng04.jpg`: the key art of the Ninja Gaiden (2004) module

Ninja Gaiden (2004)'s title screen ("PRESS START BUTTON"; the red sky, the ninja's silhouette and the logo), captured
from the user's own game running in the port (title update 1.06.04, frame 480 of run r764, 1728x972) at the author's
request (2026-11-01: "启动器切换到2004的时候，背景用ng2004标题界面的背景图，保持和ngb相同的摆放风格"). The source rectangle
x 0-1466, y 30-700 (above "PRESS START BUTTON") was placed on a 1720x860 canvas at x 254, its left and bottom 140
pixels feathered into the background's own dark red (58, 0, 0) that fills the rest, so the silhouette and the logo sit at
the top right as Ryu does in `hero.jpg`; then scaled to 1024x512 (JPEG quality 90). SHA-256 (first 20 hex):
`816d4de76dc108fe0f54`. The artwork is Koei Tecmo's.

## `icon.ico`: the programs' icon

The Dragon Sword from the game's pause menu (weapons page): its texture dumped from the user's own game with the
loader's `trace`, made into an .ico (sizes 16 to 256) for the local builds on 2026-10-22, then put into the repository
and the release at the author's decision the same day ("仍用龙剑图标"). SHA-256 (first 20 hex): `eb6e2b63165e62171b14`.
Both programs carry it through PORT_ICON (loader/CMakeLists.txt). The artwork is Koei Tecmo's.

## `pay_wechat.jpg`, `pay_alipay.jpg`: the author's payment codes

The author's own WeChat Pay and Alipay receiving codes, given on 2026-10-23 for the launcher's "支持作者" (Support) page
("如果玩家有兴趣赞助，请备注姓名"), published as they are at the author's decision ("照原样发吧"). The WeChat one is the file as
given (828x1124); the Alipay one is the given 1708x2560 image halved to 854x1280 (JPEG quality 92). At the author's request ("把支付宝二维码中间
我的照片换成另外微信中间的图片") the photo in the Alipay code's centre (box x 738-969, y 1362-1593 of the given image) was replaced
with the WeChat code's centre picture (x 365-461, y 420-515; the WeChat badge over its corner filled from the neighbouring
pixels), scaled into the box with rounded corners; no pixel outside the box changed, so the code's modules are as given. SHA-256 (first 20
hex): `pay_wechat.jpg` `d05784b833486a388ebf`, `pay_alipay.jpg` `768f8ad648ed09abcc85`.
