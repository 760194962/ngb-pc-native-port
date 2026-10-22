#include "strings.h"

Lang g_lang = Lang::Zh;

namespace {

struct Pair {
    const wchar_t *zh, *en;
};

const Pair kText[(int)Str::Count] = {
    {L"Ninja Gaiden Black·HurricaneX21[中英文版] 启动器", L"Ninja Gaiden Black·HurricaneX21 Launcher"},
    {L"飓风版 X21 · 中英文版", L"Hurricane X21 · Chinese / English"},
    {L"开始游戏", L"Play"},
    {L"安装", L"Install"},
    {L"显示", L"Display"},
    {L"图形", L"Graphics"},
    {L"控制", L"Controls"},
    {L"存档", L"Saves"},
    {L"关于", L"About"},
    {L"忍者龙剑传  黑之章", L"NINJA GAIDEN BLACK"},
    {L"飓风版 · 中英文版 · PC 原生移植", L"Hurricane edition · Chinese / English · native PC port"},
    {L"开始游戏", L"PLAY"},
    {L"还没有找到游戏：请先在「安装」页安装。", L"The game is not installed yet: see Install."},
    {L"游戏", L"Game"},
    {L"未安装", L"Not installed"},
    {L"显卡", L"Graphics card"},
    {L"自动（首次启动时选用独立显卡）", L"Automatic (the dedicated card from the first start)"},
    {L"独立显卡（高性能）", L"Dedicated (high performance)"},
    {L"集成显卡（省电）", L"Integrated (power saving)"},
    {L"画面", L"Display"},
    {L"全屏", L"Fullscreen"},
    {L"窗口", L"Windowed"},
    {L"非官方粉丝移植 · 需要自备游戏镜像 · 启动器 0.1", L"Unofficial fan port · your own game image required · launcher 0.1"},
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
    {L"已核对：支持的版本（default.xbe 与 4 个附带程序）", L"Checked: the supported build (default.xbe and 4 bundled programs)"},
    {L"不是支持的版本：", L"Not the supported build: "},
    {L"选择已有游戏…", L"Use an installed copy…"},
    {L"不是 Xbox 光盘镜像", L"Not an Xbox disc image"},
    {L"Xbox 光盘镜像 (*.iso)", L"Xbox disc image (*.iso)"},
    {L"选择你自己的《忍者龙剑传 黑之章 飓风版》x2.1 中英文版光盘镜像（.iso）。extract-xiso 做的精简镜像和完整的 redump 镜像都可以。\n\n"
     L"安装会把镜像里的全部文件（约 5.9 GB）写到下方「游戏安装路径」的文件夹，写完后核对 default.xbe 与 4 个附带程序（街机版、FC 与 SFC 三部曲）的 SHA-256，"
     L"确认是支持的版本。\n\n"
     L"安装中可以随时取消；已经写出的文件会留在文件夹里，再次安装时会被覆盖。\n\n"
     L"核对通过后，可以把镜像移到回收站来节省空间；以后需要时，还能从回收站还原。",
     L"Choose your own disc image (.iso) of NINJA GAIDEN BLACK, Hurricane edition x2.1 (Chinese / English). Both an "
     L"extract-xiso image and a full redump image will do.\n\n"
     L"Installing writes every file of the image (about 5.9 GB) to the game folder below, then checks the SHA-256 of "
     L"default.xbe and the 4 bundled programs (the arcade version, the NES and SNES trilogies) to make sure this is the "
     L"supported build.\n\n"
     L"You can cancel at any time; the files written so far stay in the folder and are overwritten by the next install.\n\n"
     L"Once the check passes you can move the image to the Recycle Bin to save space, and restore it from there later."},
    {L"这里是游戏所在的文件夹：从镜像安装时装到这里，开始游戏时也从这里启动。\n\n"
     L"点「更改…」选一个文件夹：如果里面已经有解包好的游戏（有 default.xbe），就直接使用，不需要重新安装；"
     L"如果是空文件夹，就可以从上方的镜像安装到这里。「默认路径」会改回启动器旁边的 game\\x21 文件夹。\n\n"
     L"核对会计算 default.xbe 与 4 个附带程序的 SHA-256：金色的「已核对」表示是支持的 x2.1 版本；红色表示缺少文件或是别的版本，"
     L"游戏可能无法启动。\n\n"
     L"存档不在游戏文件夹里，更换路径不会影响存档。",
     L"This is the game's folder: an install from an image goes here, and the game starts from here.\n\n"
     L"Choose a folder with \"Change\": if the game is already extracted there (a folder with default.xbe in it), it "
     L"is used as it is, with no need to install again; if the folder is empty, install into it from an image above. "
     L"\"Default\" goes back to the game\\x21 folder next to the launcher.\n\n"
     L"The check computes the SHA-256 of default.xbe and the 4 bundled programs: a gold \"Checked\" line means the "
     L"supported x2.1 build; red means a file is missing or another build, and the game may not start.\n\n"
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
    {L"21:9 与 16:10 开发中", L"21:9 and 16:10 in progress"},
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
    {L"倍数越高越清晰，也越吃显卡。宽度最多 8192（Direct3D 9 的上限）。", L"Higher is sharper and heavier on the graphics card. The width tops out at 8192 (Direct3D 9's limit)."},
    {L"带边框的窗口，可拖动、改大小", L"A framed window you can move and resize"},
    {L"独占屏幕，按桌面分辨率显示", L"Takes over the screen at the desktop resolution"},
    {L"铺满屏幕的无边框窗口，切换程序更快", L"Borderless, fills the screen; quicker to switch programs"},
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
    {L"防止画面撕裂，但偶尔会多掉几帧", L"Prevents tearing; a few more late frames"},
    {L"性能参数", L"Performance overlay"},
    {L"游戏画面右上角显示帧率、显卡、CPU、内存与磁盘", L"Frame rate, GPU, CPU, memory and disk in the game's top-right corner"},
    {L"恢复默认", L"Restore defaults"},
    {L"恢复默认设定", L"Restore the defaults"},
    {L"把「显示」页恢复为默认：窗口模式、16:9、自动窗口大小、亮度与伽马 1.00。\n\n其他页的设定不受影响。",
     L"Back to the Display page's defaults: window mode, 16:9, automatic window size, brightness and gamma 1.00.\n\nThe other pages keep their settings."},
    {L"把「图形」页恢复为默认：渲染 3×、阴影「中」、抗锯齿开、垂直同步关、填补接缝开、特效原尺寸关、性能参数关、显卡高性能。\n\n其他页的设定不受影响。",
     L"Back to the Graphics page's defaults: 3\u00D7 rendering, medium shadows, anti-aliasing on, vertical sync off, seam fill on, effects at Xbox size off, performance overlay off, the high-performance graphics card.\n\nThe other pages keep their settings."},
    {L"恢复", L"Restore"},
};

} // namespace

const wchar_t *S(Str s) { return g_lang == Lang::Zh ? kText[(int)s].zh : kText[(int)s].en; }
