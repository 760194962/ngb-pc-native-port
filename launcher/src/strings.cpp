#include "strings.h"

Lang g_lang = Lang::Zh;

namespace {

struct Pair {
    const wchar_t *zh, *en;
};

const Pair kText[(int)Str::Count] = {
    {L"忍者龙剑传 黑之章 PC 原生移植 启动器", L"Ninja Gaiden Black PC Launcher"},
    {L"PC 原生移植", L"Native PC port"},
    {L"开始游戏", L"Play"},
    {L"安装", L"Install"},
    {L"显示", L"Display"},
    {L"图形", L"Graphics"},
    {L"控制", L"Controls"},
    {L"存档", L"Saves"},
    {L"联网", L"Online"},
    {L"关于", L"About"},
    {L"支持作者", L"Support"},
    {L"忍者龙剑传  黑之章", L"NINJA GAIDEN BLACK"},
    {L"PC 原生移植 · 美版原版与汉化 / 飓风版都能玩", L"Native PC port \u00B7 the US release and its translated / Hurricane editions"},
    {L"开始游戏", L"PLAY"},
    {L"还没有找到游戏，或缺少游戏程序：请在「安装」页安装；缺少游戏程序时请重新解压下载的压缩包。",
     L"The game is not installed yet, or the game program is missing: install on the Install page; if the program is missing, extract the download again."},
    {L"游戏", L"Game"},
    {L"未安装", L"Not installed"},
    {L"显卡", L"Graphics card"},
    {L"自动（首次启动时选用独立显卡）", L"Automatic (the dedicated card from the first start)"},
    {L"独立显卡（高性能）", L"Dedicated (high performance)"},
    {L"集成显卡（省电）", L"Integrated (power saving)"},
    {L"画面", L"Display"},
    {L"全屏", L"Fullscreen"},
    {L"窗口", L"Windowed"},
    {L"非官方粉丝移植 · 需要自备游戏镜像 · 版本 %ls", L"Unofficial fan port · your own game image required · version %ls"},
    {L"此页面将在接下来的步骤中实现。", L"This page comes in the next steps."},
    {L"从镜像安装", L"Install from a disc image"},
    {L"镜像", L"Image"},
    {L"安装到", L"Install to"},
    {L"选择镜像…", L"Choose image…"},
    {L"更改…", L"Change…"},
    {L"未选择", L"None chosen"},
    {L"将安装到下方的游戏安装路径 · 需要 %.1f GB · 可用 %.1f GB", L"Installs to the game folder below · %.1f GB needed · %.1f GB free"},
    {L"空间不足", L"Not enough space"},
    {L"开始安装", L"Install"},
    {L"取消", L"Cancel"},
    {L"正在安装 %d / %d", L"Installing %d / %d"},
    {L"安装完成，已核对 default.xbe 与 4 个附带程序。", L"Installed; default.xbe and the 4 bundled programs check out."},
    {L"安装失败：", L"Install failed: "},
    {L"已取消安装。", L"Install cancelled."},
    {L"删除镜像（移到回收站）", L"Delete the image (to the Recycle Bin)"},
    {L"游戏已安装并核对无误。要把镜像移到回收站吗？", L"The game is installed and checked. Move the image to the Recycle Bin?"},
    {L"镜像已移到回收站。", L"The image is in the Recycle Bin."},
    {L"游戏安装路径", L"Game folder"},
    {L"已核对：飓风版 x2.1 中英文版（default.xbe 与 4 个附带程序）", L"Checked: Hurricane x2.1, Chinese / English (default.xbe and 4 bundled programs)"},
    {L"已核对：美版原版（default.xbe 与 2 个附带程序）", L"Checked: the original US release (default.xbe and 2 bundled programs)"},
    {L"已核对：飓风版 x2.1 日英版（default.xbe 与 4 个附带程序）", L"Checked: Hurricane x2.1, Japanese / English (default.xbe and 4 bundled programs)"},
    {L"已核对：基本版 x2 中英文版（default.xbe 与 2 个附带程序）", L"Checked: basic edition x2, Chinese / English (default.xbe and 2 bundled programs)"},
    {L"不是支持的版本：", L"Not the supported build: "},
    {L"选择已有游戏…", L"Use an installed copy…"},
    {L"不是 Xbox 光盘镜像", L"Not an Xbox disc image"},
    {L"Xbox 光盘镜像 (*.iso)", L"Xbox disc image (*.iso)"},
    {L"选择你自己的《忍者龙剑传 黑之章》光盘镜像（.iso）：飓风版 x2.1（中英 / 日英）、基本版 x2（中英），或美版原版（英文）。extract-xiso 做的精简镜像和完整的 redump 镜像都可以。\n\n"
     L"安装会把镜像里的全部文件（约 5.9 GB）写到下方「游戏安装路径」的文件夹，写完后核对 default.xbe 与附带程序（飓风版：街机版与 SFC 三部曲，日版、美版各一，共 4 个；基本版与美版原版：街机版三部曲 2 个）的 SHA-256，"
     L"确认是支持的版本。\n\n"
     L"安装中可以随时取消；已经写出的文件会留在文件夹里，再次安装时会被覆盖。\n\n"
     L"核对通过后，可以把镜像移到回收站来节省空间；以后需要时，还能从回收站还原。",
     L"Choose your own disc image (.iso) of NINJA GAIDEN BLACK: the Hurricane edition x2.1 (Chinese / English or "
     L"Japanese / English), the basic edition x2 (Chinese / English) or the original US release (English). Both an "
     L"extract-xiso image and a full redump image will do.\n\n"
     L"Installing writes every file of the image (about 5.9 GB) to the game folder below, then checks the SHA-256 of "
     L"default.xbe and the bundled programs (Hurricane: the arcade version and the SNES trilogy, a Japanese and a US build of each; "
     L"basic and US: the two arcade trilogies) to make sure this is a supported build.\n\n"
     L"You can cancel at any time; the files written so far stay in the folder and are overwritten by the next install.\n\n"
     L"Once the check passes you can move the image to the Recycle Bin to save space, and restore it from there later."},
    {L"这里是游戏所在的文件夹：从镜像安装时装到这里，开始游戏时也从这里启动。\n\n"
     L"点「更改…」选一个文件夹：如果里面已经有解包好的游戏（有 default.xbe），就直接使用，不需要重新安装；"
     L"如果是空文件夹，就可以从上方的镜像安装到这里。「默认路径」会改回启动器旁边的 game 文件夹。\n\n"
     L"核对会计算 default.xbe 与附带程序的 SHA-256：金色的「已核对」表示是支持的版本（飓风版 x2.1 中英 / 日英、基本版 x2、美版原版）；红色表示缺少文件或是别的版本，"
     L"游戏可能无法启动。\n\n"
     L"存档不在游戏文件夹里，更换路径不会影响存档。",
     L"This is the game's folder: an install from an image goes here, and the game starts from here.\n\n"
     L"Choose a folder with \"Change\": if the game is already extracted there (a folder with default.xbe in it), it "
     L"is used as it is, with no need to install again; if the folder is empty, install into it from an image above. "
     L"\"Default\" goes back to the game folder next to the launcher.\n\n"
     L"The check computes the SHA-256 of default.xbe and the bundled programs: a gold \"Checked\" line means a "
     L"supported build (Hurricane x2.1, basic x2 or the US release); red means a file is missing or another build, and the game may not start.\n\n"
     L"Saved games are not kept in the game folder, so changing it does not touch them."},
    {L"知道了", L"Got it"},
    {L"覆盖已安装的游戏？", L"Replace the installed game?"},
    {L"这个文件夹里已经装有核对无误的游戏。重新安装会用镜像里的文件覆盖它们（存档不受影响）。\n\n要继续吗？",
     L"This folder already holds a game that checks out. Installing again replaces its files with the image's (saved "
     L"games are not touched).\n\nContinue?"},
    {L"文件夹不是空的", L"The folder is not empty"},
    {L"这个文件夹里已经有其他文件。安装会覆盖同名的文件，其余的文件保留不动。\n\n如果里面是别的版本，两个版本的文件会混在一起，建议换一个空文件夹。\n\n要继续吗？",
     L"This folder already has files in it. Installing replaces those with the same names and leaves the rest.\n\nIf "
     L"another build is in there, the two would be mixed: an empty folder is better.\n\nContinue?"},
    {L"删除镜像？", L"Delete the image?"},
    {L"继续", L"Continue"},
    {L"移到回收站", L"Move to Recycle Bin"},
    {L"尚未安装：可以从上方的镜像安装到这里。", L"Not installed yet: install it here from an image above."},
    {L"默认路径", L"Default"},
    {L"显示模式", L"Display mode"},
    {L"窗口", L"Window"},
    {L"无边框", L"Borderless"},
    {L"画面比例", L"Aspect ratio"},
    {L"比 16:9 宽时，HUD、文字与菜单保持 16:9 比例；比 16:9 宽的比例为实验性。",
     L"Wider than 16:9, the HUD, text and menus keep a 16:9 shape; shapes wider than 16:9 are experimental."},
    {L"渲染分辨率", L"Render resolution"},
    {L"跟随窗口", L"Window size"},
    {L"当前：Xbox 分辨率的 %g 倍", L"Now: %g\u00D7 the Xbox resolution"},
    {L"抗锯齿", L"Anti-aliasing"},
    {L"开", L"On"},
    {L"关", L"Off"},
    {L"亮度", L"Brightness"},
    {L"伽马", L"Gamma"},
    {L"填补模型接缝", L"Fill geometry seams"},
    {L"特效保持原始分辨率", L"Effects at Xbox size"},
    {L"显卡", L"Graphics card"},
    {L"高性能", L"High performance"},
    {L"省电", L"Power saving"},
    {L"由 Windows 决定", L"Let Windows decide"},
    {L"设定会在下次启动游戏时生效。", L"Settings take effect the next time the game starts."},
    {L"显示设定", L"Display settings"},
    {L"全屏", L"Fullscreen"},
    {L"窗口大小", L"Window size"},
    {L"自动", L"Auto"},
    {L"渲染", L"Rendering"},
    {L"按窗口或屏幕的实际像素", L"The window's or screen's own pixels"},
    {L"（原版）", L" (original)"},
    {L"（默认）", L" (default)"},
    {L"倍数越高越清晰，也越吃显卡。数字是游戏内部画面的像素（主机画面横向拉伸显示，16:9 内部再加宽 4/3），不是屏幕比例。宽度最多 8192。", L"Higher is sharper and heavier on the GPU. Sizes are the internal picture, shown stretched sideways (16:9: 4/3 wider), not the screen's shape. Max width 8192."},
    {L"带边框的窗口，可拖动、改大小", L"A framed window you can move and resize"},
    {L"独占屏幕，按桌面分辨率显示", L"Takes over the screen at the desktop resolution"},
    {L"铺满屏幕的无边框窗口", L"Borderless, fills the screen"},
    {L"宽屏", L"Widescreen"},
    {L"原版画面比例", L"The original shape"},
    {L"按屏幕大小自动选择", L"Picked to fit the screen"},
    {L"超出屏幕", L"Larger than the screen"},
    {L"窗口大小只用于窗口模式；全屏与无边框按屏幕大小显示。", L"The window size is for window mode; fullscreen and borderless use the screen's size."},
    {L"在游戏自己的亮度设定之上再调整；1.00 为不调整。", L"Applied on top of the game's own brightness setting; 1.00 changes nothing."},
    {L"阴影质量", L"Shadow quality"},
    {L"低", L"Low"},
    {L"中", L"Medium"},
    {L"高", L"High"},
    {L"极高", L"Ultra"},
    {L"Xbox 原版", L"As on the Xbox"},
    {L"默认", L"Default"},
    {L"阴影贴图的分辨率：越高影子边缘越平滑，也越吃显卡。", L"The shadow map's resolution: higher gives smoother shadow edges and costs more GPU time."},
    {L"垂直同步", L"Vertical sync"},
    {L"仅独占全屏有效，防止画面撕裂", L"Exclusive fullscreen only; prevents tearing"},
    {L"性能参数", L"Performance overlay"},
    {L"游戏画面右上角显示帧率、显卡、CPU、内存与磁盘", L"Frame rate, GPU, CPU, memory and disk in the game's top-right corner"},
    {L"恢复默认", L"Restore defaults"},
    {L"恢复默认设定", L"Restore the defaults"},
    {L"把「显示」页恢复为默认：窗口模式、16:9、自动窗口大小、全屏分辨率跟随屏幕、亮度与伽马 1.00。\n\n按「保存」后才会生效；其他页的设定不受影响。",
     L"Back to the Display page's defaults: window mode, 16:9, automatic window size, fullscreen at the screen\u2019s size, brightness and gamma 1.00.\n\nIt takes effect once saved; the other pages keep their settings."},
    {L"把「图形」页恢复为默认：渲染 3×、阴影「中」、抗锯齿 FXAA、纹理过滤关、垂直同步关、填补接缝开、特效原尺寸关、性能参数关、显卡高性能。\n\n按「保存」后才会生效；其他页的设定不受影响。",
     L"Back to the Graphics page's defaults: 3\u00D7 rendering, medium shadows, FXAA anti-aliasing, texture filtering off, vertical sync off, seam fill on, effects at Xbox size off, performance overlay off, the high-performance graphics card.\n\nIt takes effect once saved; the other pages keep their settings."},
    {L"恢复", L"Restore"},
    {L"保存", L"Save"},
    {L"放弃更改", L"Discard"},
    {L"有未保存的更改：按「保存」后才会生效。", L"Unsaved changes: they take effect once saved."},
    {L"设定还没保存", L"Settings not saved"},
    {L"你改过的设定还没保存，游戏只会使用已保存的设定。\n\n要先保存再开始游戏吗？",
     L"Your changes are not saved yet, and the game uses only saved settings.\n\nSave them before starting the game?"},
    {L"你改过的设定还没保存。\n\n关闭启动器前要保存吗？", L"Your changes are not saved yet.\n\nSave them before closing the launcher?"},
    {L"不保存", L"Don\u2019t save"},
    {L"存档", L"Saves"},
    {L"还原", L"Restore"},
    {L"位置", L"Location"},
    {L"内容", L"Contents"},
    {L"还没有存档。", L"No saves yet."},
    {L"共 %d 个存档，最近一次：%ls（%ls）", L"%d saves; the latest: %ls (%ls)"},
    {L"打开文件夹", L"Open folder"},
    {L"备份", L"Backups"},
    {L"立即备份", L"Back up now"},
    {L"打开备份文件夹", L"Open backups"},
    {L"还没有备份。", L"No backups yet."},
    {L"%d 个存档", L"%d saves"},
    {L"手动备份", L"Backed up by hand"},
    {L"还原前自动备份的存档", L"Saves before a restore"},
    {L"导入前自动备份的存档", L"Saves before an import"},
    {L"导出前 xRevive 原有的存档", L"xRevive\u2019s saves before an export"},
    {L"与 xRevive 互通", L"xRevive"},
    {L"xRevive 文件夹", L"xRevive folder"},
    {L"这里没有 xRevive 的忍龙存档", L"no Ninja Gaiden saves of xRevive here"},
    {L"选择…", L"Choose\u2026"},
    {L"从 xRevive 导入", L"Import from xRevive"},
    {L"导出到 xRevive", L"Export to xRevive"},
    {L"游戏正在运行：请先关闭游戏，再备份、还原、导入或导出存档。", L"The game is running: close it before backing up, restoring, importing or exporting saves."},
    {L"已备份到 %ls", L"Backed up to %ls"},
    {L"已还原。原来的存档移到了 %ls", L"Restored. The saves that were there are now in %ls"},
    {L"已导入。原来的存档移到了 %ls", L"Imported. The saves that were there are now in %ls"},
    {L"已导出。xRevive 原有的存档移到了 %ls", L"Exported. xRevive\u2019s saves that were there are now in %ls"},
    {L"失败：%ls", L"Failed: %ls"},
    {L"还原备份", L"Restore a backup"},
    {L"用这份备份替换现在的存档吗？\n\n现在的存档不会被删除，会先整个移到一份新的备份里（标注「还原前自动备份的存档」），之后还能再还原回来。",
     L"Replace the saves now in use with this backup?\n\nThe saves now in use are not deleted: they are first moved into a new backup (\u201Csaves before a restore\u201D) and can be restored again."},
    {L"用 xRevive 的存档替换这里的存档吗？\n\n这里现在的存档不会被删除，会先移到一份新的备份里（标注「导入前自动备份的存档」）。",
     L"Replace the saves here with xRevive\u2019s?\n\nThe saves here are not deleted: they are first moved into a new backup (\u201Csaves before an import\u201D)."},
    {L"用这里的存档替换 xRevive 的存档吗？\n\nxRevive 原有的存档不会被删除，会先移到这里的备份文件夹（标注「导出前 xRevive 原有的存档」）。",
     L"Replace xRevive\u2019s saves with the ones here?\n\nxRevive\u2019s saves are not deleted: they are first moved into the backups here (\u201CxRevive\u2019s saves before an export\u201D)."},
    {L"游戏把存档写在模拟的 Xbox 硬盘里：位置下的 Partition1\\UDATA\\5443000d（每个存档格一个文件夹）与 TDATA\\5443000d（系统存档）。"
     L"位置在 launcher.ini 的 hdd 里设定，默认是启动器旁的 savedata 文件夹。\n\n存档是标准 Xbox 格式；复制整个 savedata 文件夹就是完整备份。",
     L"The game writes its saves to the emulated Xbox hard disk: Partition1\\UDATA\\5443000d under the location (one folder "
     L"per slot) and TDATA\\5443000d (the system save). The location is launcher.ini\u2019s hdd, by default the savedata "
     L"folder beside the launcher.\n\nThe saves are in the standard Xbox format; a copy of the whole savedata folder is a full backup."},
    {L"忍者龙剑传 黑之章 / 忍者龙剑传（2004）· PC 原生移植", L"Ninja Gaiden Black / Ninja Gaiden (2004) \u00B7 native PC port"},
    {L"《忍者龙剑传 黑之章》与《忍者龙剑传》（2004）（均为初代 Xbox）的非官方 Windows 原生移植，在左上角切换。"
     L"黑之章支持的光盘镜像：美版原版（英文）；基本版 va\u2011zh\u2011x2（中英）；飓风版 vb\u2011zh\u2011x2.1（中英）、vb\u2011jp\u2011x2.1（日英）。"
     L"2004 支持光盘版（也可用 Redump 完整镜像），可选装飓风包。需要你自己合法取得的游戏镜像。",
     L"An unofficial native Windows port of Ninja Gaiden Black and Ninja Gaiden (2004) for the original Xbox; switch at the top "
     L"left. Black: the US release (English), va\u2011zh\u2011x2 (Chinese / English), Hurricane vb\u2011zh\u2011x2.1 (Chinese / English), "
     L"vb\u2011jp\u2011x2.1 (Japanese / English). 2004: the disc (Redump too), optional Hurricane Packs. Your own legally obtained game image is required."},
    {L"版本", L"Version"},
    {L"启动器 %ls · 游戏程序 %ls", L"Launcher %ls \u00B7 game program %ls"},
    {L"网站", L"Website"},
    {L"作者", L"Author"},
    {L"发布页", L"Downloads"},
    {L"GitHub", L"GitHub"},
    {L"B 站", L"bilibili"},
    {L"邮箱", L"Email"},
    {L"打开网站", L"Open"},
    {L"授权", L"Licence"},
    {L"GNU 通用公共许可证第 2 版或更新版本（见 LICENSE）", L"GNU General Public License, version 2 or later (see LICENSE)"},
    {L"法律声明", L"Legal"},
    {L"本项目与 Koei Tecmo、Team NINJA 及 Microsoft 无关，也未获其认可。Ninja Gaiden 是 Koei Tecmo 的商标，Xbox 是 Microsoft 的商标。"
     L"本程序不含游戏代码或数据，也不含 Microsoft Xbox 开发包的代码。请使用自己合法取得的游戏。\n\n"
     L"例外是启动器的背景图（取自作者自己的网站，其中隼龙的主视觉版权属于 Koei Tecmo）与程序图标（游戏里龙剑的贴图），仅作为本非商业粉丝移植的界面使用。",
     L"This project is not affiliated with or endorsed by Koei Tecmo, Team NINJA or Microsoft. Ninja Gaiden is a trademark of Koei Tecmo; "
     L"Xbox is a trademark of Microsoft. This program contains no game code or data and no code from the Microsoft Xbox Development Kit. "
     L"Please use your own legally obtained copy of the game.\n\n"
     L"The exceptions are the launcher\u2019s background art, taken from the author\u2019s own website (its Ryu Hayabusa key art is "
     L"Koei Tecmo\u2019s), and the programs\u2019 icon, the game\u2019s Dragon Sword texture, used only as the interface of this "
     L"non-commercial fan port."},
    {L"缓存", L"Cache"},
    {L"CPU 核心分配", L"CPU cores"},
    {L"按窗口大小柔化边缘，几乎不耗性能", L"edges smoothed at the window\u2019s size, almost free"},
    {L"多重采样，倍数越高越平滑、越耗显卡", L"multisampling: higher is smoother and costs more"},
    {L"MSAA 最高 8×：Direct3D 9 下显卡提供的就是 2×、4×、8×。", L"MSAA up to 8\u00D7: the counts graphics cards offer under Direct3D 9 are 2\u00D7, 4\u00D7 and 8\u00D7."},
    {L"渲染 3× 以上再开 MSAA 4×/8×，集显可能掉帧；建议降低渲染倍数或用 MSAA 2×", L"MSAA 4\u00D7/8\u00D7 at a 3\u00D7 render scale or more may drop frames on an integrated GPU; lower the render scale or use 2\u00D7"},
    {L"单核（默认）", L"One core (default)"},
    {L"自动分配（实验性）", L"Automatic (experimental)"},
    {L"单核最接近 Xbox，也最稳定。自动分配让游戏线程分到多个性能核上，但游戏的主要计算集中在一个线程，不会更快，实测还略慢一点（第一章约 0.5 毫秒），也可能出现原版没有的问题；遇到异常请改回单核。下次启动游戏时生效。",
     L"One core is closest to the Xbox and the most stable. Automatic spreads the game's threads over the performance cores, but most of its work is on one thread: no faster (about 0.5 ms a frame slower in chapter 1), and it may bring problems the console never had; if so, switch back. Applies from the next start."},
    {L"清理着色器缓存", L"Clear shader cache"},
    {L"删除着色器缓存。下次启动游戏会重新编译，可能需要多等一会儿。", L"Delete the shader cache. The next start will compile the shaders again and may take a little longer."},
    {L"删除启动器目录下的着色器缓存？下次启动游戏会重新编译着色器，可能需要多等一会儿。", L"Delete the shader cache in the launcher folder? The next start will compile the shaders again and may take a little longer."},
    {L"已清理着色器缓存", L"Shader cache cleared"},
    {L"清理失败：有文件正被游戏占用，请先关闭游戏。", L"Could not clear the cache: a file is in use; close the game first."},
    {L"纹理过滤", L"Texture filtering"},
    {L"关", L"Off"},
    {L"各向异性", L"Anisotropic"},
    {L"渲染倍数 1×–2× 时让远处与斜面的贴图更清楚；3× 以上几乎没有差别。", L"Sharper distant and slanted textures at 1\u00D7\u20132\u00D7 render scale; little difference from 3\u00D7 up."},
    {L"贴图被缩小时（远处、斜着看的地面和墙）怎样取样。关 = 游戏原本的线性过滤；各向异性让这些贴图更清楚、更少糊。"
     L"渲染倍数 1×–2× 时差别看得出来；3× 以上画面本身已经超采样，几乎没有差别（实测 3× 时同一画面像素完全一样）。\n\n"
     L"性能：几乎没有影响。集成显卡实测 16×：3× 渲染时 8.14 对 7.88 毫秒，1× 渲染时 6.79 对 6.97 毫秒，都在测量误差内。",
     L"How textures are sampled when they shrink (distant or slanted floors and walls). Off = the game\u2019s own linear "
     L"filtering; anisotropic keeps them sharper. It shows at 1\u00D7\u20132\u00D7 render scale; from 3\u00D7 the picture is "
     L"already supersampled and it makes next to no difference (at 3\u00D7 the same frame came out pixel for pixel the same).\n\n"
     L"Performance: next to none. Measured at 16\u00D7 on integrated graphics: 8.14 against 7.88 ms a frame at 3\u00D7, 6.79 "
     L"against 6.97 ms at 1\u00D7, within measurement noise."},
    {L"键盘", L"Keyboard"},
    {L"手柄", L"Controller"},
    {L"按下按键…", L"Press a key\u2026"},
    {L"游戏正在运行：保存的设定会在下次启动游戏时生效。", L"The game is running: saved settings take effect the next time it starts."},
    {L"宽屏（16:10 显示器）", L"Widescreen (16:10 screens)"},
    {L"超宽屏（实验性）", L"Ultrawide (experimental)"},
    {L"点按键框，再按下新的键；Delete 清除，Esc 取消。一个键只对应一个输入。按住 Alt 时按键不会送进游戏。",
     L"Click a key box, then press the new key; Delete clears, Esc cancels. A key belongs to one input only. Keys do not reach the game while Alt is held."},
    {L"左边是游戏收到的按钮，右边选择由手柄的哪个按钮触发。十字键与摇杆不能重映射。",
     L"On the left the button the game gets, on the right the controller button that presses it. The D-pad and sticks cannot be remapped."},
    {L"已改动", L"Changed"},
    {L"把「控制」页恢复为默认：键盘用默认键位（WASD 移动、方向键视角、J/K 攻击、空格跳……），手柄按钮不重映射。\n\n按「保存」后才会生效；其他页的设定不受影响。",
     L"Back to the Controls page\u2019s defaults: the default keys (WASD to move, arrows for the camera, J/K to attack, Space to jump\u2026) and no remapped controller buttons.\n\nIt takes effect once saved; the other pages keep their settings."},
    {L"游戏本身只认手柄。这里的每个按键会直接按下手柄上对应的输入：按钮、扳机、十字键，或把摇杆推到底。每个输入可以设两个键。"
     L"键盘与手柄可以同时使用；只有游戏窗口在最前面时按键才有效。\n\n性能：没有影响（按键状态在游戏读手柄时一起读取，开销可以忽略）。",
     L"The game itself only reads its controller. Each key here simply presses that controller input: a button, a trigger, a D-pad "
     L"direction, or a stick pushed all the way. Every input can have two keys. Keyboard and controller work together; keys count only "
     L"while the game\u2019s window is in front.\n\nPerformance: none (the keys are read along with the controller, at a negligible cost)."},
    {L"指定游戏里的每个按钮由手柄上的哪个按钮来按，例如把防御移到 LB/RB 习惯的位置。要交换两个按钮（如 A 与 B），两行都要改：A 行选 B，B 行选 A；只改一行时，那个手柄按钮会同时按下两个游戏按钮。改动只对这个游戏有效，不影响 Windows 和其他游戏。\n\n"
     L"Xbox 手柄的黑键 / 白键对应 PC 手柄的 RB / LB（肩键）。\n\n"
     L"支持的手柄：Xbox 与其他 XInput 手柄；PS3 / PS4 / PS5 手柄（PS3 需装驱动）、Switch Pro 手柄、Joy-Con（一对或单只）、"
     L"DirectInput 手柄与老手柄的 USB 转接器。按键按位置对应 Xbox 布局：最下面的键是 A、右边的键是 B，PS 手柄的触摸板按下等于 Back。",
     L"Chooses which controller button presses each of the game\u2019s buttons, for example moving guard to where your hands expect it. To "
     L"swap two buttons (A and B, say) change both rows: B for A and A for B; with one row changed, that controller button presses "
     L"both game buttons. It applies to this game only, not to "
     L"Windows or other games.\n\nThe Xbox controller\u2019s Black / White buttons are the PC controller\u2019s RB / LB (shoulder buttons).\n\n"
     L"Supported controllers: Xbox and other XInput pads; PS3 / PS4 / PS5 controllers (PS3 needs its driver), the Switch Pro "
     L"Controller, Joy-Cons (a pair or one), DirectInput pads and USB adapters for older pads. Buttons follow the Xbox layout by "
     L"position: the bottom one is A, the right one B; a PlayStation pad\u2019s touchpad click is Back."},
    {L"A 键", L"A"},
    {L"B 键", L"B"},
    {L"X 键", L"X"},
    {L"Y 键", L"Y"},
    {L"黑键", L"Black"},
    {L"白键", L"White"},
    {L"左扳机 LT", L"Left trigger"},
    {L"右扳机 RT", L"Right trigger"},
    {L"Start", L"Start"},
    {L"Back", L"Back"},
    {L"左摇杆按下", L"Left stick click"},
    {L"右摇杆按下", L"Right stick click"},
    {L"十字键 上", L"D-pad up"},
    {L"十字键 下", L"D-pad down"},
    {L"十字键 左", L"D-pad left"},
    {L"十字键 右", L"D-pad right"},
    {L"移动 上（左摇杆）", L"Move up (left stick)"},
    {L"移动 下", L"Move down"},
    {L"移动 左", L"Move left"},
    {L"移动 右", L"Move right"},
    {L"视角 上（右摇杆）", L"Camera up (right stick)"},
    {L"视角 下", L"Camera down"},
    {L"视角 左", L"Camera left"},
    {L"视角 右", L"Camera right"},
    {L"「立即备份」把本作的存档复制到启动器旁的 savedata_backups 文件夹，每份以日期时间命名。列表里是最近 3 份备份，「还原」用选中的那份替换现在的存档；更早的备份在「打开备份文件夹」里。\n\n"
     L"还原、导入、导出都不会删除任何存档：被替换的存档会先移到一份新的备份里。游戏运行中不能备份、还原、导入或导出。",
     L"\u201CBack up now\u201D copies the game\u2019s saves into the savedata_backups folder beside the launcher, one dated folder "
     L"each. The 3 newest are listed; \u201CRestore\u201D replaces the saves in use with the chosen one (older backups are in "
     L"\u201COpen backups\u201D).\n\nRestoring, importing and exporting never delete a save: what is replaced is first moved "
     L"into a new backup. None of these runs while the game is running."},
    {L"xRevive 是另一个 PC 版移植器，用同样的 Xbox 存档格式，存档在它的游戏文件夹下 SaveData\\Ninja Gaiden_ Black\\HddData。"
     L"选择 xRevive 的游戏文件夹后，可以把那边的存档导入这里，或把这里的存档导出过去（已实测互通）。\n\n"
     L"被替换的一方都会先备份到这里的 savedata_backups，不会丢失。",
     L"xRevive is another PC port that uses the same Xbox save format, under its game folder\u2019s SaveData\\Ninja Gaiden_ "
     L"Black\\HddData. Choose xRevive\u2019s game folder to import its saves here or export these to it (tested to work both "
     L"ways).\n\nWhichever side is replaced is first backed up to savedata_backups here, so nothing is lost."},
    {L"窗口：带边框的窗口，可拖动、改大小。\n全屏：独占屏幕，按桌面的分辨率与刷新率显示。\n无边框：铺满屏幕的无边框窗口。\n游戏中按 Alt+Enter 在窗口与全屏之间切换：这里选的是「全屏」时切到独占全屏，否则切到无边框；退出游戏时记住最后的状态。\n\n性能：模式本身几乎没有影响。但渲染分辨率选「跟随窗口」时，画面越大（全屏、无边框）要画的像素越多，越吃显卡。",
     L"Window: a framed window you can move and resize.\nFullscreen: takes over the screen at the desktop\u2019s resolution and refresh rate.\nBorderless: a borderless window over the whole screen.\nIn the game, Alt+Enter switches between a window and fullscreen: exclusive fullscreen when \u201CFullscreen\u201D is chosen here, borderless otherwise; the game keeps the last one when it quits.\n\nPerformance: the mode itself costs next to nothing. With the render resolution at \u201CWindow size\u201D, a bigger picture (fullscreen, borderless) means more pixels to draw and more GPU time."},
    {L"画面的形状：16:9、16:10、21:9 宽屏，三种带鱼屏（64:27、43:18、12:5），4:3 原版，或「跟随显示器」。下方「窗口大小」的选项会跟着改变。\n"
     L"比 16:9 宽时游戏的 3D 视野左右变宽（上下不变），16:10 时左右略窄；HUD、文字、菜单、标题画面与片头视频保持 16:9 的比例："
     L"比 16:9 宽时放在中央、左右留黑边，16:10 时上下留黑边。比 16:9 宽的比例为实验性：商店、游戏结束等画面还没有逐一检查。\n"
     L"带鱼屏多数不正好是 21:9：5120×2160、2560×1080 选 64:27，3440×1440 选 43:18，3840×1600 选 12:5，3D 画面正好铺满；"
     L"「跟随显示器」按这台电脑主显示器的比例。\n\n"
     L"性能：同样的渲染倍数下，画面越宽像素越多（3× 时 16:9 为 2880×1440，21:9 为 3780×1440，多约 31%），稍吃显卡。",
     L"The picture\u2019s shape: 16:9, 16:10 or 21:9 widescreen, three ultrawide shapes (64:27, 43:18, 12:5), the original 4:3, "
     L"or \u201CMatch the monitor\u201D. The window sizes below follow it.\n"
     L"Wider than 16:9 the game\u2019s 3D view widens (the height stays), at 16:10 it is a little narrower; the HUD, text, "
     L"menus, the title screen and the intro movie keep a 16:9 shape: when wider in the centre with black at the sides, at "
     L"16:10 with black above and below. Shapes wider than 16:9 are experimental: screens such as the shop and game over "
     L"have not all been checked yet.\nMost ultrawides are not exactly 21:9: choose 64:27 for 5120\u00D72160 and "
     L"2560\u00D71080, 43:18 for 3440\u00D71440, 12:5 for 3840\u00D71600, and the 3D picture fills them; "
     L"\u201CMatch the monitor\u201D takes this PC\u2019s main monitor\u2019s shape.\n\n"
     L"Performance: at the same render scale a wider picture has more pixels (at 3\u00D7, 2880\u00D71440 for 16:9 and "
     L"3780\u00D71440 for 21:9, about 31% more): a little more GPU time."},
    {L"窗口模式下游戏画面的大小。「自动」选能放进桌面可用区域（不含任务栏）九成以内的最大尺寸；比可用区域大的尺寸不能选。全屏与无边框按屏幕大小显示，不使用这一项（下面「全屏分辨率」选「窗口大小」时除外）。\n\n性能：只有渲染分辨率选「跟随窗口」时，窗口越大越吃显卡；其他情况下窗口大小只影响最后的缩放，几乎没有代价。",
     L"The picture\u2019s size in window mode. \u201CAuto\u201D picks the largest within 90% of the desktop\u2019s usable area (without the taskbar); sizes larger than that area cannot be chosen. Fullscreen and borderless use the screen\u2019s size instead (unless \u201CFullscreen size\u201D below is \u201CWindow size\u201D).\n\nPerformance: only with the render resolution at \u201CWindow size\u201D does a bigger window cost more GPU time; otherwise the window size only changes the final scaling, which costs next to nothing."},
    {L"把整个画面按比例调亮或调暗。它在游戏自己的亮度设定之上再调整，1.00 为不调整；调得太高时，亮的地方会过曝变白。\n\n性能：没有影响，这一步每帧本来就会做。",
     L"Makes the whole picture brighter or darker in proportion. It applies on top of the game\u2019s own brightness setting; 1.00 changes nothing. Set too high, bright areas wash out to white.\n\nPerformance: none; this step runs every frame anyway."},
    {L"主要调整中间调与暗部，纯黑与纯白基本不变，适合「暗处看不清」的情况。它在游戏自己的亮度设定之上再调整，1.00 为不调整。\n\n性能：没有影响，这一步每帧本来就会做。",
     L"Mostly lifts or lowers the mid-tones and shadows; pure black and white stay as they are. Good when dark areas are hard to see. It applies on top of the game\u2019s own brightness setting; 1.00 changes nothing.\n\nPerformance: none; this step runs every frame anyway."},
    {L"游戏 3D 画面实际画出来的像素数，画好后再缩放到窗口或屏幕。倍数是相对 Xbox 原版的 720×480；「跟随窗口」按窗口的实际像素来画。倍数越高，边缘越锐利、细节越清楚。\n\n性能：这是最影响性能的一项，像素数随倍数的平方增加（2× 是原版的 4 倍像素，4× 是 16 倍）。集成显卡（Iris Xe）实测：3× 流畅；6× 在简单画面还行；10× 几乎每帧都卡。低配电脑建议 2×–3×，独立显卡可以选更高。",
     L"How many pixels the game\u2019s 3D picture is drawn with before it is scaled to the window or screen. The scale is relative to the Xbox\u2019s 720\u00D7480; \u201CWindow size\u201D draws at the window\u2019s own pixels. Higher is sharper.\n\nPerformance: the setting that matters most. Pixels grow with the square of the scale (2\u00D7 is 4 times the pixels, 4\u00D7 16 times). Measured on integrated graphics (Iris Xe): 3\u00D7 runs smoothly, 6\u00D7 is fine in simple scenes, 10\u00D7 stutters almost every frame. Low-end machines: 2\u00D7\u20133\u00D7; a dedicated card can go higher."},
    {L"Ryu 等角色的影子所用阴影贴图的分辨率。低 = Xbox 原版 128×128，影子边缘有明显锯齿；中（默认）以上边缘平滑，高与极高只再细一点。\n\n性能：影响很小。集成显卡实测低 / 中 / 极高的平均帧时间是 10.1 / 9.3 / 9.6 毫秒，差别在测量误差之内。",
     L"The resolution of the shadow map behind the characters\u2019 shadows. Low = the Xbox\u2019s 128\u00D7128, with jagged shadow edges; medium (the default) and up are smooth, high and ultra only a little finer.\n\nPerformance: very small. Measured on integrated graphics: 10.1 / 9.3 / 9.6 ms a frame on average at low / medium / ultra, within measurement noise."},
    {L"让物体边缘的锯齿变平滑。\n\nFXAA（默认）：画面缩放到窗口之后按窗口大小做的一道柔化，几乎不耗性能。渲染画面达到窗口的 2 倍以上时，缩小本身就是超采样（每个屏幕像素取它覆盖的全部渲染像素的平均），FXAA 不再额外处理。\n\nMSAA 2×/4×/8×：多重采样，3D 画面的边缘在绘制时就每像素取多个采样，比 FXAA 清晰，不糊贴图。Direct3D 9 下显卡提供的最高就是 8×。和低渲染倍数（1×–2×）搭配效果最好；3× 以上的渲染本身已经超采样，再开 MSAA 差别很小、代价却随倍数变大。开 MSAA 时「填补模型接缝」不起作用。\n\n性能（集成显卡实测，第一章，帧时间中位数）：3× 渲染时关 6.1 毫秒、2× 5.7、4× 10.7、8× 23.8（游戏变慢）；1× 渲染时 8× 与关闭相同（4.65 毫秒）。",
     L"Smooths jagged edges.\n\nFXAA (the default): a filter at the window’s size after the picture is scaled to it, almost free. When the 3D picture is at least twice the window’s size, the scaling down is supersampling by itself (each window pixel is the average of every rendered pixel it covers) and FXAA is not applied.\n\nMSAA 2×/4×/8×: multisampling: the 3D picture’s edges are drawn with several samples a pixel, sharper than FXAA and without blurring textures. 8× is the most graphics cards offer under Direct3D 9. It suits a low render scale (1×–2×) best; from 3× the rendering is already supersampled, so MSAA adds little and costs more the higher it goes. Seam fill does nothing while MSAA is on.\n\nPerformance (integrated GPU, chapter 1, median frame time): at 3× render scale off 6.1 ms, 2× 5.7, 4× 10.7, 8× 23.8 (the game slows down); at 1×, 8× costs nothing (4.65 ms)."},
    {L"只在「独占全屏」下起作用：等显示器刷新时才换画面，防止画面横向撕裂。窗口和无边框全屏的画面由 Windows 桌面合成显示，本来就不会撕裂，这时这一项不起作用。\n\n性能：独占全屏下实测几乎没有代价（第一关同一段路的游戏速度）：集显 60 Hz 屏幕 96.9%（关闭时 97.0%），集显 165 Hz 96.6%（关闭时 96.6%），独显 60 Hz 97.1%（关闭时 97.1%）。",
     L"Works only in exclusive fullscreen: the picture changes only when the display refreshes, so it never tears horizontally. In a window or borderless fullscreen Windows' desktop compositor shows the picture and it never tears anyway; this setting does nothing there.\n\nPerformance: measured at almost no cost in exclusive fullscreen (game speed over the same stretch of chapter 1): integrated GPU at 60 Hz 96.9% (97.0% off), at 165 Hz 96.6% (96.6% off), dedicated GPU at 60 Hz 97.1% (97.1% off)."},
    {L"把模型拼接处在高渲染倍数下露出的细小裂缝补起来（由周围的颜色填上）。在本游戏中实测作用很小：第 1 章一帧只改了约 415 万像素中的 72 个，放大也看不出差别。保持开启即可。\n\n渲染画面达到窗口的 3 倍以上时，一条裂缝缩小后最多只占屏幕像素的三分之一，会被平均掉，这一项自动不做。\n\n性能：影响很小。",
     L"Fills the hairline cracks that open where model pieces meet at high render scales (with the colour around them). In this game it does very little: one chapter-1 frame had 72 of about 4.1 million pixels changed, invisible even zoomed in. It can stay on.\n\nWhen the 3D picture is at least three times the window’s size, a crack is a third of a window pixel at most once scaled down and is averaged away; this step is then skipped.\n\nPerformance: very small."},
    {L"光晕、灯光、模糊这些特效原本画在 320×240、160×120 之类的小缓冲区里。默认（关）把它们和画面一起放大并平滑；打开则保持 Xbox 原来的大小，光晕更柔、颗粒更粗，更接近原版主机的样子。\n\n性能：影响很小，打开反而稍省。集成显卡实测平均帧时间 7.97 对 7.63 毫秒，差别在测量误差内。",
     L"Glow, light and blur effects are drawn into small buffers (320\u00D7240, 160\u00D7120 and so on). Off (the default) scales them with the picture and smooths them; on keeps them at the Xbox\u2019s size: softer, coarser glows, closer to the original console.\n\nPerformance: very small; on is slightly cheaper. Measured on integrated graphics: 7.63 against 7.97 ms a frame on average, within measurement noise."},
    {L"在游戏画面右上角显示：帧率、显卡与 CPU 的名称和使用率（CPU 另外列出游戏本身的占用）、内存、磁盘。\n\n性能：几乎没有影响。实测每帧只多 0.02 毫秒；读取这些数据在一个最低优先级的后台线程里，每秒一次，每次约 2–3 毫秒。",
     L"Shows in the game\u2019s top-right corner: frame rate, the graphics card and processor with their use (the processor also the game\u2019s own share), memory and disk.\n\nPerformance: next to none. Measured: 0.02 ms a frame; the figures are read on a lowest-priority thread once a second, about 2\u20133 ms each time."},
    {L"选择游戏用哪张显卡（Windows「图形设置」里按程序指定的偏好）。列表只列出能选到的卡；默认是高性能那张。\n\n性能：独立显卡通常快很多，但在笔记本上更耗电、更热；集成显卡省电，但高渲染倍数时容易卡。",
     L"Which graphics card the game uses (Windows\u2019 per-program graphics preference). Only the cards that can be chosen are listed; the default is the high-performance one.\n\nPerformance: a dedicated card is usually much faster but uses more power and runs hotter on a laptop; integrated graphics saves power but stutters sooner at high render scales."},
    {L"联网排行榜",
     L"Online leaderboards"},
    {L"联网",
     L"Online"},
    {L"xemu EEPROM",
     L"xemu EEPROM"},
    {L"xemu 硬盘镜像",
     L"xemu hard disk"},
    {L"可导入的账号",
     L"To import"},
    {L"导入用：选 xemu 里注册过 Insignia 的 EEPROM 和硬盘镜像",
     L"To import: the EEPROM and hard disk registered in xemu"},
    {L"✓ 主机身份有效 · 已注册 Insignia",
     L"✓ Console identity valid · registered with Insignia"},
    {L"  ·  账号：",
     L"  ·  Accounts: "},
    {L"EEPROM 无效（不是 Xbox 的 EEPROM 文件？）",
     L"The EEPROM is not valid (not an Xbox EEPROM file?)"},
    {L"读不到 EEPROM 文件",
     L"Cannot read the EEPROM file"},
    {L"读不了硬盘镜像：",
     L"Cannot read the hard disk image: "},
    {L"这台 xemu 主机没联网注册过，没有可导入的账号",
     L"This xemu console was never registered to play online: nothing to import"},
    {L"xemu 正在运行：开始游戏前请关掉它（同一台主机不能同时登录两次）",
     L"xemu is running: close it before playing (one console cannot sign in twice)"},
    {L"检查不了（找不到游戏程序？）",
     L"Cannot check (is the game program missing?)"},
    {L"Xbox 文件",
     L"Xbox files"},
    {L"DNS",
     L"DNS"},
    {L"当前 DNS",
     L"DNS in use"},
    {L"自动  ",
     L"Automatic  "},
    {L"手动  ",
     L"Manual  "},
    {L"查询方式",
     L"Lookups"},
    {L"Insignia 公网 DNS",
     L"Insignia public DNS"},
    {L"本地 InsigniaDNS.exe",
     L"Local InsigniaDNS.exe"},
    {L"推荐：Insignia 架在网上的服务器，不用装任何程序。",
     L"Recommended: Insignia’s server on the internet; nothing to run."},
    {L"开着代理 TUN 时用：查询转给下面这台运行 InsigniaDNS.exe 的电脑。",
     L"With a proxy’s TUN on: lookups go to the PC below, running the InsigniaDNS.exe program."},
    {L"检查网络",
     L"Check network"},
    {L"正在检查…",
     L"Checking…"},
    {L"✓ DNS 正常：AS.XBOXLIVE.COM → ",
     L"✓ DNS works: AS.XBOXLIVE.COM → "},
    {L"DNS 被代理的 TUN 截走（回答 %s）：办法见 DNS 旁的「?」",
     L"A proxy’s TUN captures DNS (answer %s): see the “?” by DNS"},
    {L"%s 没有回应：检查网络",
     L"%s does not answer: check the network"},
    {L"%s 没有回应：那台电脑上 InsigniaDNS.exe 开着吗？",
     L"%s does not answer: is InsigniaDNS.exe running on that PC?"},
    {L"DNS 有回答，但没有 AS.XBOXLIVE.COM 的地址",
     L"DNS answered without an address for AS.XBOXLIVE.COM"},
    {L"登录",
     L"Signing in"},
    {L"进游戏后：主菜单「排行榜」→ 选自己的玩家代号，登录和榜单都在游戏里。还没有账号：看「注册步骤」，在 xemu 里或直接在移植版里注册。",
     L"In the game: main menu → Ranking → your gamertag; signing in happens there. No account yet: see How to register (in xemu, or right here in the port)."},
    {L"Insignia 官网",
     L"Insignia website"},
    {L"联网关闭，DNS 查询方式改回「Insignia 公网 DNS」。选好的 EEPROM 与硬盘镜像保留。",
     L"Online off and lookups back to Insignia’s public DNS. The chosen EEPROM and hard disk image stay."},
    {L"用你自己的 Insignia 账号在游戏「排行榜」里登录、查看真实榜单。移植版用的是你在 xemu 里注册好的那台「主机」：点「从 xemu 导入」，它的主机身份（EEPROM）和账号会复制进存档目录（online_eeprom.bin、online_accounts.bin），游戏只读这两个文件。以后在 xemu 里新建了账号，再导入一次就会补进来，本机已有的账号不受影响。xemu 的文件只读，不会被修改，也不会上传到别处。\n\n存档目录里的 online_eeprom.bin 就是你的主机身份：不要把它发给别人。\n\n默认关闭；关闭时游戏看不到网络，和以前完全一样。xemu 在运行时不会开联网，因为同一台主机不能同时登录两次。\n\n性能：集成显卡第 1 章实测，开与关都是 97.7% 的游戏速度，网络只在进入排行榜后才启动。",
     L"Sign in with your own Insignia account in the game’s Ranking menu and see the real leaderboards. The port uses the “console” you registered in xemu: Import copies its identity (the EEPROM) and its accounts into the save folder (online_eeprom.bin, online_accounts.bin), and the game reads only those. Accounts you make in xemu later come over with another Import; the ones here stay as they are. xemu’s files are only read, never changed or sent anywhere.\n\nonline_eeprom.bin in the save folder is your console’s identity: do not give it to anyone.\n\nOff by default; off, the game sees no network, exactly as before. While xemu runs the port stays offline, since one console cannot sign in twice.\n\nPerformance: measured on integrated graphics in chapter 1, 97.7% game speed both on and off; the network only starts once you open Ranking."},
    {L"游戏登录前要先用 DNS 查到 Insignia 服务器的地址。下面两个名字很像，但不是一回事：\n"
     L"· Insignia 公网 DNS：Insignia 架在网上的 DNS 服务器（46.101.64.175），什么都不用装。\n"
     L"· 本地 InsigniaDNS.exe：从 Insignia 下载、在你电脑上运行的小程序，在这台电脑的局域网地址（如 192.168.x.x）上回答查询，127.0.0.1 不行。\n\n"
     L"查询方式选「Insignia 公网 DNS」（推荐）：所有查询都转给它，不用开 InsigniaDNS.exe，也不管 xemu 里怎么设定。\n\n"
     L"选「本地 InsigniaDNS.exe」：所有查询转给一台运行它的电脑，地址在这里填（「找本机」自动找）；换了电脑或网络只要改这里。\n\n"
     L"开着代理（Clash 等）的 TUN 时，公网 DNS 会被截走（「检查网络」答出 198.18.x.x / 198.19.x.x）。办法：先关 TUN，启动 InsigniaDNS.exe，再开 TUN，然后选「本地 InsigniaDNS.exe」。或者直接关掉 TUN。",
     L"Before signing in the game looks up Insignia’s servers through DNS. Two things have nearly the same name:\n"
     L"· Insignia’s public DNS: Insignia’s DNS server on the internet (46.101.64.175); nothing to install.\n"
     L"· Local InsigniaDNS.exe: a small program from Insignia running on your PC; it answers on that PC’s LAN address (such as 192.168.x.x), not on 127.0.0.1.\n\n"
     L"“Insignia public DNS” (recommended): every lookup goes to it, with no InsigniaDNS.exe and whatever xemu was set to.\n\n"
     L"“Local InsigniaDNS.exe”: every lookup goes to a PC running it, at the address entered here (“This PC” finds it); on another PC or network, change it here.\n\n"
     L"With a proxy’s (Clash etc.) TUN on, public DNS is captured (Check network answers 198.18.x.x / 198.19.x.x). Turn TUN off, start InsigniaDNS.exe, turn TUN back on, then choose “Local InsigniaDNS.exe”. Or just turn TUN off."},
    {L"选择文件…", L"Choose file…"},
    {L"InsigniaDNS.exe 地址",
     L"InsigniaDNS.exe address"},
    {L"点这里填地址",
     L"Click to enter"},
    {L"找本机",
     L"This PC"},
    {L"✓ 找到本机正在运行的 InsigniaDNS.exe",
     L"✓ InsigniaDNS.exe is running on this PC"},
    {L"本机没有在运行的 InsigniaDNS.exe，先填了本机地址：记得打开它",
     L"No InsigniaDNS.exe running here; this PC’s address is filled in: start it"},
    {L"找不到本机的网络地址",
     L"No network address found for this PC"},
    {L"地址格式不对，例如 192.168.1.10",
     L"Not an address, e.g. 192.168.1.10"},
    {L"（Insignia 公网 DNS）",
     L" (Insignia’s public DNS)"},
    {L"（本地 InsigniaDNS.exe）",
     L" (local InsigniaDNS.exe)"},
    {L"（还没填 InsigniaDNS.exe 地址，先用 Insignia 公网 DNS）",
     L" (no InsigniaDNS.exe address yet: Insignia’s public DNS for now)"},
    {L"（xemu 主机设定里的）",
     L" (from the console’s setting in xemu)"},
    {L"注册步骤",
     L"How to register"},
    {L"注册 Insignia（只需一次）",
     L"Registering with Insignia (once)"},
    {L"两种方式，选一种。\n\n"
     L"■ 方式一：已经在 xemu 里注册过 Insignia\n"
     L"1. 关掉 xemu。\n"
     L"2. 在「xemu EEPROM」「xemu 硬盘镜像」两行选 xemu 用的文件（如 eeprom.bin、xbox_hdd.qcow2，位置在 xemu 设置的「系统」页）。建议先备份这两个文件。\n"
     L"3. 点「从 xemu 导入」。「本机账号」一行显示已注册和你的玩家代号就完成了。以后在 xemu 里新建了账号，再导入一次就会补进来，本机已有的不受影响。\n\n"
     L"■ 方式二：不用 xemu，直接在移植版里注册\n"
     L"要准备两样东西：Insignia 的注册程序 Insignia-Setup.iso（Insignia 官网免费下载）；以及微软的 Xbox 仪表盘（升级到 Insignia 要求的 5960 版），移植版从中取出 Xbox Live 仪表盘，它是微软的文件，移植版不能附带。仪表盘可以来自：装过官方仪表盘的 xemu 硬盘镜像（xbox_hdd.qcow2，不必注册过）；从自己的 Xbox 导出的硬盘镜像；或用 FTP 从改机 Xbox 拷出的整个 C 盘（选其中的 xboxdash.xbe）。xemu 官网的预制硬盘镜像没有微软仪表盘，不能用。\n"
     L"1. 点「新建主机」：生成一台新的 Xbox 主机，保存在存档目录里。\n"
     L"2. 点「注册主机」，第一次会让你选 Insignia-Setup.iso。注册程序打开后约 18 秒才能操作；选「Register Xbox」，看到 “Your Xbox was successfully registered” 后选「Quit Setup Assistant」退出。\n"
     L"3. 点「Xbox Live 仪表盘」，第一次会让你选上面说的硬盘镜像（或拷出的 C 盘里的 xboxdash.xbe）。进入后选「NETWORK SETUP」做一次连接测试，主机账户在这一步建立。开着代理（Clash 等）的 TUN 时这一步会失败：DNS 改用「本地 InsigniaDNS.exe」或关掉 TUN。\n"
     L"4. 在 insignia.live 首页填邮箱申请订阅码，订阅码会发到邮箱。\n"
     L"5. 在仪表盘选「NEW ACCOUNT」：选国家、取玩家代号、填订阅码。服务免费；到信用卡一步按官网说明填测试卡号 4111 1111 1111 1111 和假资料，不要填真卡；账单邮箱要和申请订阅码时一样。最后设一个通行码。\n"
     L"6. 退出仪表盘（BACK 退到底，或直接关窗口）。「本机账号」一行显示你的玩家代号就完成了。\n\n"
     L"两种方式都不用改仪表盘里的 DNS：移植版会把查询转给下面「DNS」卡片选的服务器。具体操作以 Insignia 官网和它的 Discord 为准。",
     L"Two ways; pick one.\n\n"
     L"\u25A0 1. Registered with Insignia in xemu already\n"
     L"1. Close xemu.\n"
     L"2. Choose xemu\u2019s files in the xemu EEPROM and xemu hard disk rows (e.g. eeprom.bin and xbox_hdd.qcow2; xemu\u2019s System settings show where they are). Back them up first.\n"
     L"3. Press Import. The Accounts here row then shows registered and your gamertag. Accounts you make in xemu later come over with another Import; the ones here stay as they are.\n\n"
     L"\u25A0 2. Without xemu, in the port\n"
     L"You need two things: Insignia's Setup Assistant, Insignia-Setup.iso (free on Insignia's website), and Microsoft's Xbox dashboard (updated to 5960, as Insignia asks): the port takes the Xbox Live dashboard from it, as it is Microsoft's and cannot come with the port. It can come from an xemu hard disk image with the official dashboard (xbox_hdd.qcow2, registered or not), an image of your own Xbox's disk, or a copy of a modded Xbox's whole C: taken by FTP (choose its xboxdash.xbe). The ready-made image on xemu's website has no Microsoft dashboard and does not work.\n"
     L"1. Press New console: the port makes a new Xbox console in the save folder.\n"
     L"2. Press Register; the first time it asks for Insignia-Setup.iso. The Setup Assistant takes about 18 seconds before it answers the pad; choose Register Xbox, and after \u201CYour Xbox was successfully registered\u201D choose Quit Setup Assistant.\n"
     L"3. Press Xbox Live; the first time it asks for the hard disk image above (or the xboxdash.xbe in a copy of C:). In the dashboard choose NETWORK SETUP and run the connection test: it makes the console\u2019s machine account. With a proxy\u2019s (Clash etc.) TUN on this step fails: use \u201cLocal InsigniaDNS.exe\u201d or turn TUN off.\n"
     L"4. Enter your email on insignia.live to get a subscription code; it is emailed to you.\n"
     L"5. In the dashboard choose NEW ACCOUNT: country, gamertag, the code. The service is free: at the card step use the test card 4111 1111 1111 1111 and made-up details as the site says, never a real card; the billing email must be the one you got the code with. Last, set a pass code.\n"
     L"6. Leave the dashboard (BACK all the way, or close its window). The Accounts here row then shows your gamertag.\n\n"
     L"Neither way needs the dashboard\u2019s DNS setting changed: the port sends every lookup to the server chosen in the DNS card below. For the details, Insignia\u2019s site and Discord have the last word."},
    {L"请作者喝杯奶茶",
     L"Buy the author a milk tea"},
    {L"《黑之章》的中文汉化、飓风补丁，还有这个 PC 原生移植版（黑之章和《忍者龙剑传》2004），都是我做的。移植版从拆解原版程序做起，画面、声音、存档、联网，到最近修好的手柄震动，一点点接到了 PC 上。它们都免费，以后也一直免费，赞不赞助都不影响你玩到的任何内容。\n\n"
     L"如果它让你又一次握起了龙剑，觉得值一杯奶茶，扫下面任意一个码就好（微信、支付宝）。转账时请备注姓名或昵称，好让我知道这份心意来自谁。\n\n"
     L"不方便也完全没关系：玩得开心，遇到问题告诉我，一样是在帮它变得更好。",
     L"The Chinese translation of Ninja Gaiden Black, the Hurricane patch and this native PC port (Ninja Gaiden Black and Ninja Gaiden 2004) are all my work. The port was built up from the original program piece by piece: graphics, sound, saves, online play, and lately the controller’s rumble. All of it is free and will stay free; nothing in it depends on a donation.\n\n"
     L"If it put the Dragon Sword back in your hands and feels worth a milk tea, either code below works (WeChat Pay, Alipay). Please put your name or nickname in the note, so I know who it came from.\n\n"
     L"If not, that is perfectly fine: enjoy the game, and tell me about anything that goes wrong. That helps just as much."},
    {L"跟随显示器（3D 画面铺满这台显示器）", L"Match the monitor (the 3D picture fills it)"},
    {L"跟随显示器：超出支持范围（1.5:1 到 2.5:1）", L"Match the monitor: outside 1.5:1 to 2.5:1"},
    {L"跟随显示器", L"Monitor"},
    {L"跟随屏幕", L"The screen"},
    {L"超出屏幕：选无边框可铺满", L"Larger than the screen: borderless fills it"},
    {L"带鱼屏 5120×2160、2560×1080", L"Ultrawide 5120\u00D72160, 2560\u00D71080"},
    {L"带鱼屏 3440×1440", L"Ultrawide 3440\u00D71440"},
    {L"带鱼屏 3840×1600", L"Ultrawide 3840\u00D71600"},
    {L"完成。原来没有存档，所以没有移走任何东西。", L"Done. There were no saves there, so nothing was moved."},
    {L"全屏分辨率", L"Fullscreen size"},
    {L"窗口大小", L"Window size"},
    {L"配置较低时选它：画面按窗口大小输出再放大，更省显卡", L"For slow machines: shown at the window size, scaled up"},
    {L"全屏与无边框时画面按多大的分辨率输出。\n「跟随屏幕」（默认）：按屏幕的分辨率输出，最清晰。\n「窗口大小」：按上面「窗口大小」选的尺寸输出（保持屏幕的比例，不会变形），再放大到整个屏幕，画面会软一些，但显卡要处理的像素少。无边框时由系统放大，全屏（独占）时由显卡在交给显示器前放大，都不切换显示器的分辨率。窗口大小不小于屏幕时没有区别。\n\n性能：给配置较低的电脑用。省下的是每帧最后几道全屏处理（伽马、抗锯齿、接缝填补）；「图形」页的渲染分辨率选「跟随窗口」时，3D 画面本身也按这个尺寸画，省得更多。屏幕越大（2K、4K）省得越多。想减轻显卡负担，先调低「图形」页的渲染分辨率，效果最明显。",
     L"The resolution the picture is shown at in fullscreen and borderless.\n\u201CThe screen\u201D (default): the screen\u2019s own, the sharpest.\n\u201CWindow size\u201D: the window size chosen above (kept at the screen\u2019s shape, so nothing is stretched), scaled up to the whole screen: a little softer, but fewer pixels for the graphics card. Borderless, Windows scales it; in (exclusive) fullscreen the graphics card scales it before the display shows it. The display\u2019s resolution is not changed. With a window size no smaller than the screen nothing changes.\n\nPerformance: for slow machines. It saves the last full-screen passes of each frame (gamma, anti-aliasing, seam fill); with the render resolution at \u201CWindow size\u201D on the Graphics page the 3D picture itself is drawn at this size too, saving more. The larger the screen (1440p, 4K), the more it saves. To lighten the load, lower the render resolution on the Graphics page first: that matters most."},
    {L"全屏与无边框也按这个大小输出，再放大到整个屏幕。", L"Fullscreen and borderless show the picture at this size too, scaled up to the screen."},
    {L"黑之章", L"Black"},
    {L"2004", L"2004"},
    {L"忍者龙剑传", L"NINJA GAIDEN"},
    {L"PC 原生移植 · 2004 原版，装上飓风包可打忍者大师锦标赛", L"Native PC port \u00B7 the 2004 original; with the Hurricane Packs, the Master Ninja Tournament"},
    {L"已核对：忍者龙剑传（2004）光盘版（default.xbe 与 2 个附带程序）", L"Checked: Ninja Gaiden (2004), the disc (default.xbe and 2 bundled programs)"},
    {L"这是忍者龙剑传（2004）：请在左上角切换到「2004」", L"This is Ninja Gaiden (2004): switch to 2004 at the top left"},
    {L"这是黑之章：请在左上角切换到「黑之章」", L"This is Ninja Gaiden Black: switch to Black at the top left"},
    {L"飓风包（可选）", L"Hurricane Packs (optional)"},
    {L"安装包", L"Installer"},
    {L"安装飓风包", L"Install"},
    {L"未安装：运行光盘原版 1.02（没有飓风包和锦标赛）", L"Not installed: the disc\u2019s 1.02 runs (no Hurricane Packs, no tournament)"},
    {L"已安装：", L"Installed: "},
    {L"更新 1.06", L"update 1.06"},
    {L"飓风包 1", L"Hurricane Pack 1"},
    {L"飓风包 2", L"Hurricane Pack 2"},
    {L"飓风包已安装。开始游戏时会自动运行 1.06 更新版。", L"Installed. The game now starts with the 1.06 update."},
    {L"安装失败：", L"Could not install: "},
    {L"飓风包是 2004 年在 Xbox Live 上免费发布的追加内容（新关卡、新敌人、新武器与服装），配合「忍者大师锦标赛」。它带着游戏的 1.06 更新版。\n\n先把「Ninja Gaiden DLC Installer.rar」解压，再点「选择文件夹…」选解压出来的文件夹（选「Ninja Gaiden DLC Installer」或里面的「content」都可以），然后点「安装飓风包」。它会装进存档所在的 Xbox 硬盘（TDATA\\54430003），和真机一样。\n\n装了：开始游戏时自动运行 1.06 更新版；游戏每次启动时会按本机密钥给飓风包签名，所以不会提示「内容损坏」。锦标赛菜单需要联网：先在「联网」页配好 Insignia。\n\n没装：运行光盘原版 1.02，可以正常单机游玩。", L"The Hurricane Packs were free downloads on Xbox Live in 2004 (new stages, enemies, weapons and costumes), played in the Master Ninja Tournament. They bring the game\u2019s 1.06 update.\n\nExtract \u201CNinja Gaiden DLC Installer.rar\u201D first, then choose the extracted folder with \u201CChoose folder\u201D (\u201CNinja Gaiden DLC Installer\u201D or the \u201Ccontent\u201D inside it) and press Install. They go onto the Xbox hard disk with the saves (TDATA\\54430003), as on the console.\n\nInstalled: the game starts with the 1.06 update, and signs the packs for this console each time it starts, so they are not \u201Cdamaged\u201D. The tournament needs online play: set up Insignia on the Online page first.\n\nNot installed: the disc\u2019s 1.02 runs, fine for playing on your own."},
    {L"选择你自己的《忍者龙剑传》（2004，初代 Xbox）光盘镜像（.iso）。extract-xiso 做的精简镜像和完整的 redump 镜像都可以。\n\n安装会把镜像里的全部文件（约 5.9 GB）写到下方「游戏安装路径」的文件夹（默认是启动器旁边的 game_ng04，和黑之章分开），写完后核对 default.xbe 与 2 个附带程序（SFC 三部曲，日版、美版各一）的 SHA-256。\n\n安装中可以随时取消；核对通过后，可以把镜像移到回收站来节省空间。飓风包在下方单独安装。", L"Choose your own disc image (.iso) of NINJA GAIDEN (2004, the original Xbox). Both an extract-xiso image and a full redump image will do.\n\nInstalling writes every file of the image (about 5.9 GB) to the game folder below (by default game_ng04 next to the launcher, apart from Black), then checks the SHA-256 of default.xbe and the 2 bundled programs (the SNES trilogy, a Japanese and a US build).\n\nYou can cancel at any time; once the check passes you can move the image to the Recycle Bin. The Hurricane Packs install below."},
    {L"xRevive 导入导出只用于黑之章。", L"xRevive import and export are for Black only."},
    {L"一键重签", L"Re-sign saves"},
    {L"一键重签存档？", L"Re-sign the saves?"},
    {L"把当前游戏的存档重新签名成这台电脑的，让「另一台 Xbox / xemu / 别的玩家」的存档，以及旧版本开着联网时存的档，都能正常读取（不再显示 Damaged Game、「无法读取游戏设定」）。\n\n只改签名对不上的文件，存档内容不变。改之前会先把当前存档完整备份一份（备份列表里标「重签前」），随时可以还原。", L"Signs the current game\u2019s saves for this PC again, so saves from another Xbox, from xemu or from another player, and those made with online play on in earlier versions, load (no more Damaged Game or \u201CUnable to load game settings\u201D).\n\nOnly files whose signature is wrong change, and only their signature. The saves are backed up first (\u201Cbefore re-signing\u201D in the list), to restore at any time."},
    {L"已重签 %d 个存档文件。原来的存档已备份到：%s", L"Re-signed %d save files. The saves before are backed up in: %s"},
    {L"所有存档的签名都正确，不需要重签。", L"Every save is signed right: nothing to re-sign."},
    {L"重签前自动备份", L"Before re-signing"},
    {L"卸载内容", L"Uninstall"},
    {L"%d 项", L"%d installed"},
    {L"没有已安装的内容", L"Nothing installed"},
    {L"卸载这项内容？", L"Uninstall this content?"},
    {L"从游戏硬盘上删除「%s」（%s）。\n\n更新 1.06 和飓风包随时可以用上面的「安装飓风包」装回来；在游戏里下载的通知等内容，可以在忍者大师锦标赛的「NEW NOTICE!」里重新下载。",
     L"Deletes “%s” (%s) from the game’s hard disk.\n\nUpdate 1.06 and the Hurricane Packs come back with Install above at any time; notices and other content downloaded in the game download again from “NEW NOTICE!” in the Master Ninja Tournament."},
    {L"已卸载：%s", L"Uninstalled: %s"},
    {L"卸载失败：%s", L"Could not uninstall: %s"},
    {L"导出", L"Export"},
    {L"已导出到 %s，别的玩家在「安装包」选这个文件夹即可安装", L"Exported to %s: another player chooses that folder as the installer and installs"},
    {L"导出失败：%s", L"Could not export: %s"},
    {L"主机设置", L"Console"},
    {L"系统语言", L"System language"},
    {L"游戏按主机的系统语言选默认语言；游戏没有的语言会用英语", L"Games pick their language from the console's; one a game lacks falls back to English"},
    {L"初代 Xbox 的系统语言（主机设置里的 Language）。游戏启动时读它，按它决定默认的菜单文字、字幕或语音；"
     L"游戏里没有的语言一般会用英语。对所有游戏和镜像都生效（黑之章、忍者龙剑传 2004）。\n\n"
     L"汉化版（飓风版、基本版）是在英文文字上改成中文的，请保持「English」，换成别的语言可能显示回原来的外文。"
     L"\n\n声音：主机的模拟音频输出。单声道把所有声音混成一路；立体声为默认；杜比环绕（Dolby Surround）把后方声道编码进两路立体声，"
     L"接支持 Pro Logic 的功放可以解出后置。\n\n数字输出：Dolby Digital / DTS 打开后，游戏按 5.1 声道混音，移植版以 5.1 输出"
     L"（前左、前右、中置、低音、后左、后右）；音箱不足 6 声道时由 Windows 自动往下混。电脑只有一路输出，所以数字输出开启时「声音」一项不起作用（原机上它管的是 AV 线的模拟输出）。黑之章开 5.1 时原版会把背景音乐和过场声音"
     L"压到几乎听不见，移植版已修正。"
     L"\n\n下次开始游戏时生效。",
     L"The original Xbox's system language (Language in the console's settings). Games read it at start and pick their "
     L"menus, subtitles or voices from it; a language a game lacks usually means English. It applies to every game and "
     L"image (Black, Ninja Gaiden 2004).\n\nThe Chinese editions (Hurricane, basic) are translated over the English text: "
     L"keep \"English\"; another language may bring the original text back.\n\nSound: the console's analog output. Mono mixes everything into one channel; stereo is the default; Dolby Surround matrixes the rear channels into the two (a Pro Logic receiver takes them back out).\n\nDigital output: with Dolby Digital or DTS on, games mix for 5.1 and the port sends 5.1 out (front left / right, centre, LFE, back left / right); Windows folds it for fewer speakers. The PC has one output, so with the digital output on, Sound has no effect (on the console it is the AV cable's analog output). Black's original turns its music and movie sound almost silent on 5.1; the port fixes that.\n\nTakes effect the next time the game starts."},
    {L"声音", L"Sound"},
    {L"单声道", L"Mono"},
    {L"立体声", L"Stereo"},
    {L"杜比环绕", L"Dolby Surround"},
    {L"数字输出", L"Digital output"},
    {L"两者", L"Both"},
    {L"数字输出开启时以 5.1 输出，此项不起作用", L"Digital output on: 5.1 out, this has no effect"},
    {L"影子柔化", L"Soft shadows"},
    {L"角色影子的边缘按原机显卡的方式柔化：对阴影贴图周围 2×2 个点分别比较深度再混合（PCF，原机这一代显卡在阴影贴图开线性过滤时由硬件完成），"
     L"边缘是柔和的渐变，而不是硬边和锯齿。在所选的阴影分辨率上柔化：分辨率越高，边缘越细越平滑。关闭后为以前的做法：只比较一次，边缘硬朗。\n\n"
     L"性能影响很小（只多读几次阴影贴图，只在画影子的地方）。下次开始游戏时生效。",
     L"Character shadows get the soft edge of the console's GPU: four texels of the shadow map around each pixel are "
     L"compared and blended (percentage-closer filtering, done in hardware by that generation of GPUs when the shadow "
     L"map is filtered linearly), instead of a hard, stepped edge. It works at the chosen shadow resolution: the higher, "
     L"the finer and smoother the edge. Off: the earlier way, one compare, a hard edge.\n\n"
     L"Little cost (a few more shadow-map reads, only where shadows are drawn). Takes effect the next time the game starts."},
    {L"本机主机",
     L"This console"},
    {L"✓ 可以联网",
     L"✓ Ready to play online"},
    {L"还没有：点「新建主机」，或用下面的「从 xemu 导入」",
     L"None yet: press New console, or import from xemu below"},
    {L"存档目录里的主机文件损坏或不配套：从 xemu 重新导入，或按「重建主机」",
     L"The console files in the save folder are damaged or do not match: import from xemu again, or Rebuild"},
    {L"存档目录里的账号属于另一台主机：从 xemu 重新导入，或按「重建主机」",
     L"The accounts in the save folder are another console\u2019s: import from xemu again, or Rebuild"},
    {L"还不能联网：按「注册步骤」先注册",
     L"Not ready to play online yet: register first (How to register)"},
    {L"导入账号时才用到；不选也能用本机账号联网。",
     L"Only needed to import accounts; the accounts here play online without them."},
    {L"（会转到本机主机）",
     L" (moved onto this console)"},
    {L"：",
     L": "},
    {L"✓ 账号都已在本机",
     L"✓ All its accounts are here"},
    {L"从 xemu 导入",
     L"Import"},
    {L"导入了：",
     L"Imported: "},
    {L"已有：",
     L"Already here: "},
    {L"没有空位（最多 8 个账号）：",
     L"No free slot (8 accounts at most): "},
    {L"没有要导入的账号",
     L"Nothing to import"},
    {L"导入失败：",
     L"Import failed: "},
    {L"✓ 可以导入",
     L"✓ Can be imported"},
    {L"新建主机",
     L"New console"},
    {L"注册主机",
     L"Register"},
    {L"新建一台主机？",
     L"Make a new console?"},
    {L"移植版会生成一台新的 Xbox 主机（新的序列号和密钥），保存在存档目录里。接着用 Insignia 注册程序注册它（「注册主机」），再建玩家代号。\n\n"
     L"以前在 xemu 里联网玩过的话，直接「从 xemu 导入」更省事，不用新建。新建以后，xemu 里的账号也还能导入过来（会转到这台新主机上）。",
     L"The port makes a new Xbox console (a new serial number and keys) in the save folder. Then register it with the Insignia Setup Assistant (Register) and make a gamertag.\n\n"
     L"Played online in xemu before? Import is simpler, with no new console. Made one anyway, xemu\u2019s accounts can still be imported onto it."},
    {L"新建失败：",
     L"Could not make it: "},
    {L"注册程序正在运行：在它的窗口里选「Register Xbox」，完成后选「Quit」关掉它",
     L"The Setup Assistant is running: choose Register Xbox in its window, then Quit"},
    {L"Insignia 注册程序（Insignia-Setup.iso 或其中的 default.xbe）",
     L"Insignia Setup Assistant (Insignia-Setup.iso or its default.xbe)"},
    {L"这不是 Insignia 注册程序：请从 Insignia 官网下载 Insignia-Setup.iso",
     L"That is not the Insignia Setup Assistant: get Insignia-Setup.iso from Insignia\u2019s website"},
    {L"Xbox Live \u4eea\u8868\u76d8",
     L"Xbox Live"},
    {L"\u4eea\u8868\u76d8\u6b63\u5728\u8fd0\u884c\uff1a\u505a\u5b8c\u540e BACK \u9000\u51fa\uff0c\u6216\u5173\u6389\u5b83\u7684\u7a97\u53e3",
     L"The dashboard is running: when done, BACK out of it or close its window"},
    {L"Xbox 硬盘镜像（含仪表盘，如 xbox_hdd.qcow2），或从 Xbox 拷出的 C 盘里的 xboxdash.xbe",
     L"Xbox hard disk image with the dashboard (e.g. xbox_hdd.qcow2), or xboxdash.xbe in a copy of an Xbox\u2019s C:"},
    {L"选的镜像或文件夹里\u6ca1\u6709\u79fb\u690d\u7248\u80fd\u7528\u7684\u4eea\u8868\u76d8（要 C:\\xodash 里的 5971 版 xonlinedash.xbe；拷出的 C 盘要有 xodash、fonts 和 xboxdash.xbe）",
     L"No dashboard the port can run there (it needs C:\\xodash with xonlinedash.xbe 5971; a copy of C: needs xodash, fonts and xboxdash.xbe)"},
    {L"本机账号",
     L"Accounts"},
    {L"还没有玩家代号：在「Xbox Live 仪表盘」里选 NEW ACCOUNT 新建",
     L"No gamertag yet: make one with NEW ACCOUNT in the Xbox Live dashboard"},
    {L"从 xemu 导入（以前在 xemu 里联网玩过才需要）",
     L"Import from xemu (only if you played online in xemu before)"},
    {L"校验不过、没有导入：",
     L"Did not check out, not imported: "},
    {L"仪表盘没打开：",
     L"Dashboard not opened: "},
    {L"仪表盘的联网测试没有通过，本机主机还不能联网：在仪表盘 NETWORK SETUP 里再测一次；开着代理的 TUN 时，DNS 改用「本地 InsigniaDNS.exe」或关掉 TUN（DNS 旁的「?」）",
     L"The dashboard’s network test did not get through, so the console cannot go online yet: test again in NETWORK SETUP; with a proxy’s TUN on, use “Local InsigniaDNS.exe” or turn TUN off (the “?” by DNS)"},
    {L"主机已注册。下一步：在仪表盘里做联网测试",
     L"Registered. Next: the connection test in the dashboard"},
    {L"开始游戏时会自动从下面选好的 xemu 文件导入以前的主机；也可以现在按「从 xemu 导入」",
     L"Your earlier console is imported from the xemu files below when the game starts; or press Import now"},
    {L"重建主机",
     L"Rebuild"},
    {L"重建主机",
     L"Rebuild the console"},
    {L"本机主机的文件坏了或不配套，没法再用。\n\n"
     L"重建会把旧文件改名为 .old 留在存档文件夹里，再新建一台主机。新主机要重新注册（「注册主机」和仪表盘的联网测试），以前的玩家代号要在仪表盘里按 Insignia 官网的说明找回。\n\n"
     L"以前在 xemu 里联网玩过的，用下面的「从 xemu 导入」更简单，不用重建。",
     L"This console’s files are damaged or do not match and cannot be used.\n\n"
     L"Rebuild renames the old files to .old in the save folder and makes a new console. It has to be registered again (Register, then the dashboard’s connection test); earlier gamertags are recovered in the dashboard as Insignia’s website explains.\n\n"
     L"Played online in xemu before? Import from xemu below is simpler, with no rebuild."},
};

} // namespace

const wchar_t *S(Str s) { return g_lang == Lang::Zh ? kText[(int)s].zh : kText[(int)s].en; }
