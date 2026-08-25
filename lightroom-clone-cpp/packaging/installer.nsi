; ---------------------------------------------------------------------------
; Silver Halide — NSIS installer script.
;
; Compile (from project root):
;   C:\Users\tmlde\Desktop\dev\nsis\makensis.exe packaging\installer.nsi
;
; The payload (SilverHalide.exe + Qt runtime + plugins + VC++ runtime) must be
; staged under build_gui/install_stage before compiling (exe rebuilt with the
; WIN32 GUI subsystem, then windeployqt + VC redist copy). The active build tree
; is the Ninja layout at the repository's parent directory (../build_gui, no
; Release layer); the stale VS tree at lightroom-clone-cpp/build_gui/Release is
; NOT used. See the packaging workflow notes in STATUS.md.
; ---------------------------------------------------------------------------

Unicode true

!include "MUI2.nsh"
!include "x64.nsh"

; ---------------------------------------------------------------------------
; Branding / version
; ---------------------------------------------------------------------------
!define APP_NAME      "Silver Halide"
!define APP_VERSION   "2.11.0"
!define APP_EXE       "SilverHalide.exe"
!define APP_PUBLISHER "ASE-128"
!define APP_COPYRIGHT "©ASE-128 2026"
!define APP_UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_NAME}"

; Payload is staged relative to this script file (packaging/../../build_gui =
; the active Ninja build tree in the repository's parent directory):
!define STAGE_DIR "..\..\build_gui\install_stage"

Name "${APP_NAME}"
OutFile "..\dist\Silver Halide Setup ${APP_VERSION}.exe"
InstallDir "$PROGRAMFILES64\${APP_NAME}"
InstallDirRegKey HKLM "Software\${APP_NAME}" "InstallDir"
RequestExecutionLevel admin
SetCompressor /SOLID lzma

VIProductVersion "2.11.0.0"
VIAddVersionKey "ProductName"      "${APP_NAME}"
VIAddVersionKey "CompanyName"      "${APP_PUBLISHER}"
VIAddVersionKey "LegalCopyright"   "${APP_COPYRIGHT}"
VIAddVersionKey "FileDescription"  "${APP_NAME} ${APP_VERSION} 安装程序"
VIAddVersionKey "FileVersion"      "${APP_VERSION}"
VIAddVersionKey "ProductVersion"   "${APP_VERSION}"

; ---------------------------------------------------------------------------
; MUI2 interface
; ---------------------------------------------------------------------------
!define MUI_ABORTWARNING
!define MUI_ICON "..\resources\icons\app.ico"
!define MUI_UNICON "..\resources\icons\app.ico"
!define MUI_WELCOMEFINISHPAGE_BITMAP "welcome.bmp"
!define MUI_FINISHPAGE_RUN "$INSTDIR\${APP_EXE}"
!define MUI_FINISHPAGE_RUN_TEXT "运行 ${APP_NAME}"
!define MUI_FINISHPAGE_RUN_PARAMETERS ""

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "SimpChinese"

; ---------------------------------------------------------------------------
; .onInit — require 64-bit Windows, use 64-bit registry view
; ---------------------------------------------------------------------------
Function .onInit
  SetRegView 64
  ${IfNot} ${RunningX64}
    MessageBox MB_OK|MB_ICONSTOP "${APP_NAME} 仅支持 64 位 Windows 系统。"
    Abort
  ${EndIf}
FunctionEnd

; ---------------------------------------------------------------------------
; Install section
; ---------------------------------------------------------------------------
Section "Install" SEC_APP
  SetOutPath "$INSTDIR"
  File "${STAGE_DIR}\SilverHalide.exe"
  File "${STAGE_DIR}\*.dll"

  SetOutPath "$INSTDIR\platforms"
  File "${STAGE_DIR}\platforms\*.dll"
  SetOutPath "$INSTDIR\imageformats"
  File "${STAGE_DIR}\imageformats\*.dll"
  SetOutPath "$INSTDIR\iconengines"
  File "${STAGE_DIR}\iconengines\*.dll"
  SetOutPath "$INSTDIR\generic"
  File "${STAGE_DIR}\generic\*.dll"
  SetOutPath "$INSTDIR\networkinformation"
  File "${STAGE_DIR}\networkinformation\*.dll"
  SetOutPath "$INSTDIR\styles"
  File "${STAGE_DIR}\styles\*.dll"
  SetOutPath "$INSTDIR\tls"
  File "${STAGE_DIR}\tls\*.dll"

  ; Install dir + Add/Remove Programs registry entries
  WriteRegStr HKLM "Software\${APP_NAME}" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "${APP_UNINST_KEY}" "DisplayName"     "${APP_NAME}"
  WriteRegStr HKLM "${APP_UNINST_KEY}" "DisplayVersion"  "${APP_VERSION}"
  WriteRegStr HKLM "${APP_UNINST_KEY}" "DisplayIcon"     "$INSTDIR\${APP_EXE}"
  WriteRegStr HKLM "${APP_UNINST_KEY}" "Publisher"       "${APP_PUBLISHER}"
  WriteRegStr HKLM "${APP_UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${APP_UNINST_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegDWORD HKLM "${APP_UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${APP_UNINST_KEY}" "NoRepair" 1

  ; Start menu + desktop shortcuts
  CreateDirectory "$SMPROGRAMS\${APP_NAME}"
  CreateShortcut "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}"
  CreateShortcut "$DESKTOP\${APP_NAME}.lnk"                "$INSTDIR\${APP_EXE}"

  WriteUninstaller "$INSTDIR\Uninstall.exe"
SectionEnd

; ---------------------------------------------------------------------------
; Uninstall section
; ---------------------------------------------------------------------------
Section "Uninstall"
  SetRegView 64

  Delete "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk"
  RMDir  "$SMPROGRAMS\${APP_NAME}"
  Delete "$DESKTOP\${APP_NAME}.lnk"

  Delete "$INSTDIR\Uninstall.exe"
  Delete "$INSTDIR\${APP_EXE}"
  Delete "$INSTDIR\*.dll"
  Delete "$INSTDIR\platforms\*.dll"
  Delete "$INSTDIR\imageformats\*.dll"
  Delete "$INSTDIR\iconengines\*.dll"
  Delete "$INSTDIR\generic\*.dll"
  Delete "$INSTDIR\networkinformation\*.dll"
  Delete "$INSTDIR\styles\*.dll"
  Delete "$INSTDIR\tls\*.dll"
  RMDir  "$INSTDIR\platforms"
  RMDir  "$INSTDIR\imageformats"
  RMDir  "$INSTDIR\iconengines"
  RMDir  "$INSTDIR\generic"
  RMDir  "$INSTDIR\networkinformation"
  RMDir  "$INSTDIR\styles"
  RMDir  "$INSTDIR\tls"
  RMDir  "$INSTDIR"

  DeleteRegKey HKLM "${APP_UNINST_KEY}"
  DeleteRegKey HKLM "Software\${APP_NAME}"
SectionEnd
