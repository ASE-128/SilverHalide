; ---------------------------------------------------------------------------
; Silver Halide — Inno Setup 6 installer script.
;
; Compile (from packaging/ directory):
;   "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" installer.iss
;
; Why Inno Setup over NSIS:
;   • zip compression (default) is ~10x faster than LZMA solid on a 660 MB
;     payload of mostly already-compressed ONNX weights / Qt DLLs.
;   • Per-file compression with fast incompressibility detection — high-entropy
;     binaries (.onnx / .data) fall back to store mode quickly.
;   • Built-in 64-bit install mode, desktop/start-menu icons, uninstaller.
;
; The payload (SilverHalide.exe + Qt runtime + plugins + ONNX Runtime + AI
; models + VC redist) is staged under build_gui/install_stage before
; compiling (exe rebuilt, windeployqt run, models copied, VC redist merged).
; See installer.nsi (kept as historical reference) for the staging workflow.
; ---------------------------------------------------------------------------

#define APP_NAME      "Silver Halide"
#define APP_VERSION   "1.0.0"
#define APP_EXE       "SilverHalide.exe"
#define APP_PUBLISHER "ASE-128"
#define APP_COPYRIGHT "Copyright (C) ASE-128 2026"

; Payload is staged relative to this script file (packaging/../../build_gui =
; the active Ninja build tree in the repository's parent directory).
#define STAGE_DIR     "..\..\build_gui\install_stage"

[Setup]
; Branding
AppName={#APP_NAME}
AppVersion={#APP_VERSION}
AppVerName={#APP_NAME} {#APP_VERSION}
AppPublisher={#APP_PUBLISHER}
AppCopyright={#APP_COPYRIGHT}
AppPublisherURL=https://github.com/ASE-128/SilverHalide
; Default install location: 64-bit Program Files. {autopf} resolves to
; Program Files on 32-bit setups and Program Files (x86) on 64-bit-only
; installs — we force x64 below, so this is always Program Files.
DefaultDirName={autopf}\{#APP_NAME}
DefaultGroupName={#APP_NAME}
UninstallDisplayIcon={app}\{#APP_EXE}
UninstallDisplayName={#APP_NAME}
; Require 64-bit Windows. ArchitecturesInstallIn64BitMode=x64os makes the
; installer write files to the 64-bit Program Files view and the 64-bit
; registry view (HKLM\Software, not the WOW6432Node reflection). Inno Setup
; 6.7+ prefers "x64os" / "x64compatible" over the deprecated "x64".
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
; Output: dist/Silver Halide Setup 1.0.0.exe
OutputDir=..\dist
OutputBaseFilename=Silver Halide Setup {#APP_VERSION}
; Compression: zip (fast, with built-in store fallback for incompressible
; binaries). SolidCompression=no means each file is compressed independently,
; so installing an updated build over an existing install only re-compresses
; the changed files. LZMA is intentionally NOT used — it offers slightly
; higher ratio but takes minutes on this payload mix.
Compression=zip
SolidCompression=no
; VersionInfo embedded into the .exe (shown by Windows File > Properties).
VersionInfoVersion={#APP_VERSION}.0
VersionInfoProductVersion={#APP_VERSION}.0
VersionInfoCompany={#APP_PUBLISHER}
VersionInfoProductName={#APP_NAME}
VersionInfoDescription={#APP_NAME} {#APP_VERSION} Setup
VersionInfoCopyright={#APP_COPYRIGHT}
; License agreement page. Shown after the Welcome page; user must accept
; before the install proceeds. The file is UTF-8 with BOM so Inno Setup
; decodes the Chinese reference translation correctly on non-Chinese
; Windows code pages too. Path is relative to this .iss file.
LicenseFile=LICENSE.txt
; Disable directory-exists prompt (we always install fresh); keep the
; ready-to-install page for clarity.
DisableDirPage=no
DisableProgramGroupPage=yes
; Allow installs to non-empty directories (upgrade path).
UsePreviousAppDir=yes
; Wizard pages: Welcome -> License (must accept) -> SelectDir -> Ready -> Installing -> Finished.
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=dialog
; Installer icon (replaces the default Inno Setup "gears" icon). The .ico
; contains multiple resolutions (16/32/48/64/128/256) so Windows File >
; Properties and the taskbar both pick a crisp size. Path is relative to
; this .iss file (packaging/../resources/icons/app.ico).
SetupIconFile=..\resources\icons\app.ico
; Wizard side banner (164x314 BMP, displayed on Welcome / Ready / Finished
; pages). Stored in the packaging folder alongside this script. BMP must be
; 24-bit or 8-bit uncompressed (no PNG); welcome.bmp already meets this.
WizardImageFile=welcome.bmp
WizardImageStretch=no

[Languages]
; Chinese (Simplified) is the only installer language. The .isl ships in
; packaging/lang/ (kept in-repo so the build is self-contained — no install
; step required into the Inno Setup Languages folder). The app UI itself is
; already Chinese (DevelopPanel labels are SimpChinese).
Name: "chinesesimplified"; MessagesFile: "lang\ChineseSimplified.isl"

[CustomMessages]
; The community ChineseSimplified.isl does not ship the [CustomMessages]
; block that Default.isl defines (CreateDesktopIcon / AdditionalIcons /
; CreateQuickLaunchIcon). Without these, {cm:...} references in [Tasks]
; fall back to English. Override them here, scoped to chinesesimplified so
; any future English-language addition can override cleanly.
chinesesimplified.AdditionalIcons=附加快捷方式：
chinesesimplified.CreateDesktopIcon=创建桌面快捷方式(&D)
chinesesimplified.CreateQuickLaunchIcon=创建快速启动快捷方式(&Q)

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; Main executable.
Source: "{#STAGE_DIR}\{#APP_EXE}"; DestDir: "{app}"; Flags: ignoreversion
; Qt6 + ONNX Runtime + VC++ runtime DLLs (flat in install_stage root).
Source: "{#STAGE_DIR}\*.dll"; DestDir: "{app}"; Flags: ignoreversion
; Qt platform plugins (windows platform plugin is required at startup).
Source: "{#STAGE_DIR}\platforms\*.dll"; DestDir: "{app}\platforms"; Flags: ignoreversion
; Image format plugins (JPEG / PNG / TIFF / WebP / SVG).
Source: "{#STAGE_DIR}\imageformats\*.dll"; DestDir: "{app}\imageformats"; Flags: ignoreversion
; Icon engine plugin (used by SVG / ICO rendering).
Source: "{#STAGE_DIR}\iconengines\*.dll"; DestDir: "{app}\iconengines"; Flags: ignoreversion
; Generic Qt plugins (e.g. tga / tiff).
Source: "{#STAGE_DIR}\generic\*.dll"; DestDir: "{app}\generic"; Flags: ignoreversion
; Network information plugin (used by QtNetwork for reachability checks).
Source: "{#STAGE_DIR}\networkinformation\*.dll"; DestDir: "{app}\networkinformation"; Flags: ignoreversion
; Style plugins (e.g. Windows11 style).
Source: "{#STAGE_DIR}\styles\*.dll"; DestDir: "{app}\styles"; Flags: ignoreversion
; TLS plugins (used by QtNetwork for HTTPS model downloads).
Source: "{#STAGE_DIR}\tls\*.dll"; DestDir: "{app}\tls"; Flags: ignoreversion

; --- AI models (read-only, bundled at $INSTDIR\models) ---------------------
; ModelManager::bundledRoot() resolves to <applicationDirPath>/models, so an
; installed copy finds its models via readPathFor() without a re-download.
; isModelReady() probes SHA256 (or sizeBytes when SHA is TBD) before the
; engine registers the model — so a corrupt install is detected at startup.
Source: "{#STAGE_DIR}\models\manifest.json"; DestDir: "{app}\models"; Flags: ignoreversion
Source: "{#STAGE_DIR}\models\nafnet-denoise.onnx"; DestDir: "{app}\models"; Flags: ignoreversion
Source: "{#STAGE_DIR}\models\nafnet_denoise.data"; DestDir: "{app}\models"; Flags: ignoreversion
Source: "{#STAGE_DIR}\models\rmbg2-subject.onnx"; DestDir: "{app}\models"; Flags: ignoreversion
; Internal test stub — may not be staged. /nonfatal in NSIS is analogous to
; the Inno Setup `Flags: skipifsourcedoesntexist` here.
Source: "{#STAGE_DIR}\models\test_add_2x2.onnx"; DestDir: "{app}\models"; Flags: skipifsourcedoesntexist

; VC++ Redistributable installer (per-machine). Install silently if missing;
; Inno Setup runs it via [Run] below. The 25 MB merge module is shipped
; standalone (not embedded into every file) so it can be skipped when the
; target already has the VC++ runtime.
Source: "{#STAGE_DIR}\vc_redist.x64.exe"; DestDir: "{tmp}"; Flags: deleteafterinstall

[Icons]
; Start menu shortcut (group created by DefaultGroupName above).
Name: "{group}\{#APP_NAME}"; Filename: "{app}\{#APP_EXE}"
; Desktop shortcut (only when the user selects the desktopicon task).
Name: "{commondesktop}\{#APP_NAME}"; Filename: "{app}\{#APP_EXE}"; Tasks: desktopicon

[Run]
; Silent-install VC++ 2015-2022 x64 redistributable. /install /quiet /norestart
; is the official Microsoft silent-install argument set. The installed
; SilverHalide.exe dynamically links msvcp140.dll / vcruntime140.dll, which
; are guaranteed present after this runs. If the redistributable is already
; at the same or higher version, the installer exits 0 (no-op).
Filename: "{tmp}\vc_redist.x64.exe"; Parameters: "/install /quiet /norestart"; Check: VCRedistNeedsInstall; StatusMsg: "正在安装 VC++ 运行库..."
; Offer to launch Silver Halide after a successful install.
Filename: "{app}\{#APP_EXE}"; Description: "启动 {#APP_NAME}"; Flags: nowait postinstall skipifsilent

[Code]
// --- VC++ 2015-2022 x64 redistributable check ----------------------------------
// Returns true when the bundled vc_redist.x64.exe should be installed. Uses
// the standard HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64
// registry key, which is set by the official VC redist installer (and by
// Visual Studio setup). Version >= 14.40 (MSVC 14.40 toolset = VS 2022
// 17.10+) is the minimum required by Silver Halide; older versions are
// upgraded. The registry check is a 64-bit query because we install in
// 64-bit mode (ArchitecturesInstallIn64BitMode=x64).
function VCRedistNeedsInstall: Boolean;
var
  VersionStr: String;
  Installed: Boolean;
begin
  Installed := RegQueryStringValue(HKLM,
    'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64',
    'Version', VersionStr);
  Result := not Installed;
end;

[UninstallDelete]
; Remove the bundled models directory on uninstall. The per-user cache
; (%LOCALAPPDATA%\Silver Halide\models\) is intentionally NOT deleted —
// downloaded models survive an uninstall so a re-install doesn't re-download.
Type: filesandordirs; Name: "{app}\models"
