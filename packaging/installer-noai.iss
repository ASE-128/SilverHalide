; ---------------------------------------------------------------------------
; Silver Halide — Inno Setup 6 installer script (NO-AI variant).
;
; Compile (from packaging/ directory):
;   "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" installer-noai.iss
;
; Variant differences from installer.iss:
;   • Payload comes from build_gui_noai/install_stage (configured with
;     -DLRC_ENABLE_AI=OFF), so neither ONNX Runtime nor the AI models are
;     shipped.
;   • The AI entry points (AI denoise controls, AI mask buttons, 设置 → 模型
;     管理…) are compiled out with #if LRC_ENABLE_AI, so the installed UI has
;     no AI affordances at all.
;   • D3D11 GPU pixel backend (LRC_ENABLE_D3D=ON) is still included.
;
; The payload (SilverHalide.exe + Qt runtime + plugins + VC redist) is staged
; under build_gui_noai/install_stage before compiling (exe rebuilt, windeployqt
; run, VC redist copied). DCP camera profiles and the Lensfun profile database
; are sourced directly from build_gui_noai/resources (not install_stage) so the
; installer always packages the full current set.
; ---------------------------------------------------------------------------

#define APP_NAME      "Silver Halide"
#define APP_VERSION   "1.0.0"
#define APP_EXE       "SilverHalide.exe"
#define APP_PUBLISHER "ASE-128"
#define APP_COPYRIGHT "Copyright (C) ASE-128 2026"

; Payload is staged relative to this script file (packaging/../build_gui_noai).
#define STAGE_DIR     "..\build_gui_noai\install_stage"
; Camera DCP profiles and the Lensfun database live under build_gui_noai/resources.
#define RES_DIR       "..\build_gui_noai\resources"

[Setup]
; Branding
AppName={#APP_NAME}
AppVersion={#APP_VERSION}
AppVerName={#APP_NAME} {#APP_VERSION} (No AI)
AppPublisher={#APP_PUBLISHER}
AppCopyright={#APP_COPYRIGHT}
AppPublisherURL=https://github.com/ASE-128/SilverHalide
DefaultDirName={autopf}\{#APP_NAME}
DefaultGroupName={#APP_NAME}
UninstallDisplayIcon={app}\{#APP_EXE}
UninstallDisplayName={#APP_NAME} (No AI)
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
; Output: dist/Silver Halide Setup 1.0.0 (No AI).exe
OutputDir=..\dist
OutputBaseFilename=Silver Halide Setup {#APP_VERSION} (No AI)
Compression=lzma2/ultra
LZMADictionarySize=32
SolidCompression=yes
LZMAUseSeparateProcess=yes
LZMANumBlockThreads=4
VersionInfoVersion={#APP_VERSION}.0
VersionInfoProductVersion={#APP_VERSION}.0
VersionInfoCompany={#APP_PUBLISHER}
VersionInfoProductName={#APP_NAME}
VersionInfoDescription={#APP_NAME} {#APP_VERSION} (No AI) Setup
VersionInfoCopyright={#APP_COPYRIGHT}
LicenseFile=LICENSE.txt
DisableDirPage=no
DisableProgramGroupPage=yes
UsePreviousAppDir=yes
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=dialog
SetupIconFile=..\resources\icons\app.ico
WizardImageFile=welcome.bmp
WizardImageStretch=no

[Languages]
Name: "chinesesimplified"; MessagesFile: "lang\ChineseSimplified.isl"

[CustomMessages]
chinesesimplified.AdditionalIcons=附加快捷方式：
chinesesimplified.CreateDesktopIcon=创建桌面快捷方式(&D)
chinesesimplified.CreateQuickLaunchIcon=创建快速启动快捷方式(&Q)

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; Main executable.
Source: "{#STAGE_DIR}\{#APP_EXE}"; DestDir: "{app}"; Flags: ignoreversion
; Qt6 runtime + VC++ runtime DLLs (flat in install_stage root). The no-AI tree
; contains no onnxruntime*.dll by construction.
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

; --- Adobe DCP camera profiles (grouped by camera model) -------------------
Source: "{#RES_DIR}\Camera\*"; DestDir: "{app}\resources\Camera"; Flags: ignoreversion recursesubdirs createallsubdirs

; --- Lensfun profile database ---------------------------------------------
Source: "{#RES_DIR}\lensfun\version_1\*"; DestDir: "{app}\share\lensfun\version_1"; Flags: ignoreversion recursesubdirs createallsubdirs

; VC++ Redistributable installer (per-machine), run via [Run] below.
Source: "{#STAGE_DIR}\vc_redist.x64.exe"; DestDir: "{tmp}"; Flags: deleteafterinstall

[Icons]
Name: "{group}\{#APP_NAME}"; Filename: "{app}\{#APP_EXE}"
Name: "{commondesktop}\{#APP_NAME}"; Filename: "{app}\{#APP_EXE}"; Tasks: desktopicon

[Run]
Filename: "{tmp}\vc_redist.x64.exe"; Parameters: "/install /quiet /norestart"; Check: VCRedistNeedsInstall; StatusMsg: "正在安装 VC++ 运行库..."
Filename: "{app}\{#APP_EXE}"; Description: "启动 {#APP_NAME}"; Flags: nowait postinstall skipifsilent

[Code]
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
; Bundled Lensfun profile database (camera/lens calibrations).
Type: filesandordirs; Name: "{app}\share"
; Remove the per-user model cache, which may have been created by another
; installer variant.
Type: filesandordirs; Name: "{localappdata}\Silver Halide\models"
