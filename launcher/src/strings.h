// The launcher's text in Simplified Chinese and English.
#pragma once

enum class Lang { Zh, En };
extern Lang g_lang;

enum class Str {
    WindowTitle,
    Edition,
    NavStart, // the navigation, in page order
    NavInstall,
    NavDisplay,
    NavControls,
    NavSaves,
    NavAbout,
    Title,
    Subtitle,
    Play,
    NotReady,
    Game,
    GameMissing,
    Gpu,
    GpuAuto,
    GpuDedicated,
    GpuIntegrated,
    Display,
    Fullscreen,
    Windowed,
    Footer,
    ComingSoon,
    // the install page
    FromImage,
    Image,
    InstallTo,
    Choose,
    Change,
    NotChosen,
    Space,        // %.1f GB needed, %.1f GB free
    NoSpace,
    StartInstall,
    Cancel,
    Installing,   // %d / %d files
    InstallDone,
    InstallFailed,
    InstallCancelled,
    DeleteImage,
    DeleteConfirm,
    ImageDeleted,
    InstalledGame,
    GameChecked,
    GameWrong,
    ChooseExisting,
    NotAnImage,
    ImageFilter,
    HelpFromImage, // the "?" panels: their text
    HelpInstalled,
    GotIt,
    AskOverwriteTitle, // the confirmations
    AskOverwrite,
    AskNonEmptyTitle,
    AskNonEmpty,
    AskDeleteTitle,
    Continue,
    MoveToBin,
    NotInstalledHere,
    DefaultPath,
    Count
};

const wchar_t *S(Str s);
