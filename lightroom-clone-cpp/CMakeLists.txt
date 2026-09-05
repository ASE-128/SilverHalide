cmake_minimum_required(VERSION 3.25)
project(lightroom_clone_cpp LANGUAGES C CXX VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ---------------------------------------------------------------------------
# MSVC runtime library & CRT path workaround
# ---------------------------------------------------------------------------
# The Ninja+MSVC generator relies on the LIB environment variable (set by a VS
# developer command prompt) to find the CRT libraries.  When cmake is run from
# a plain shell (Git Bash, pwsh, …) the LIB variable is absent and the linker
# cannot resolve _fltused, memset, memcmp, operator new[], etc.
#
# We explicitly set the runtime library policy and, when LIB is missing, inject
# the MSVC CRT and Windows SDK UCRT library directories so the linker can find
# msvcrt.lib / msvcprt.lib / libucrt.lib.
if(MSVC)
  if(NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL" CACHE STRING
      "MSVC runtime library (MultiThreadedDLL = /MD)" FORCE)
  endif()

  # When LIB is not set, the linker search path does not include the CRT
  # directories.  We derive them from the detected compiler toolchain.
  if(NOT DEFINED ENV{LIB})
    # Derive MSVC root directory from the compiler path:
    #   .../MSVC/14.xx.xxxxx/bin/Hostx64/x64/cl.exe
    get_filename_component(_msvc_bin_dir  "${CMAKE_CXX_COMPILER}" DIRECTORY)  # .../bin/Hostx64/x64
    get_filename_component(_msvc_bin_dir  "${_msvc_bin_dir}"     DIRECTORY)  # .../bin/Hostx64
    get_filename_component(_msvc_host_dir "${_msvc_bin_dir}"     DIRECTORY)  # .../bin
    get_filename_component(_msvc_root     "${_msvc_host_dir}"    DIRECTORY)  # .../14.xx.xxxxx
    set(_msvc_lib_dir "${_msvc_root}/lib/x64")
    if(EXISTS "${_msvc_lib_dir}")
      link_directories("${_msvc_lib_dir}")
      message(STATUS "MSVC CRT lib dir: ${_msvc_lib_dir}")
    else()
      message(WARNING "Could not find MSVC CRT library directory at ${_msvc_lib_dir}")
    endif()

    # Windows SDK UCRT library path — pick the newest version available.
    file(GLOB _ucrt_dirs
      "C:/Program Files (x86)/Windows Kits/10/Lib/*/ucrt/x64"
      "C:/Program Files (x86)/Windows Kits/*/Lib/*/ucrt/x64")
    if(_ucrt_dirs)
      list(SORT _ucrt_dirs)
      list(REVERSE _ucrt_dirs)
      list(GET _ucrt_dirs 0 _ucrt_dir)
      link_directories("${_ucrt_dir}")
      message(STATUS "Windows UCRT lib dir: ${_ucrt_dir}")
    else()
      message(WARNING "Could not find Windows SDK UCRT library directory")
    endif()
  endif()

  # /EHsc: enable C++ exception unwind semantics. Without it MSVC compiles
  # try/catch but never catches (C4530) and a throw calls std::terminate,
  # which silently breaks Parallel.cpp's exception guard. (C++ exceptions.)
  add_compile_options(/EHsc)
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(LRC_BUILD_TESTS "Build the Catch2 test suite" ON)
option(LRC_ENABLE_QT "Build the Qt6 GUI" ON)
option(LRC_USE_OPENCV "Enable OpenCV auxiliary helpers (heavy dependency)" OFF)
option(LRC_ENABLE_AI "Enable AI features (ONNX Runtime)" ON)
option(LRC_ENABLE_LENSFUN "Enable Lensfun lens correction profiles" ON)

# ---------------------------------------------------------------------------
# External dependencies. Two acquisition strategies:
#   1. vcpkg  -> set CMAKE_TOOLCHAIN_FILE to vcpkg's toolchain (see README).
#   2. FetchContent from official hosts, with optional GitHub proxy mirror
#      for restricted networks: -DLRC_PROXY=https://ghfast.top/
# The order below prefers already-installed CMake packages, then falls back.
# ---------------------------------------------------------------------------
include(FetchContent)
set(FETCHCONTENT_BASE_DIR "${CMAKE_BINARY_DIR}/_deps")

# GitHub proxy helper: prepends LRC_PROXY to github.com URLs.
set(LRC_PROXY "" CACHE STRING "GitHub proxy prefix, e.g. https://ghfast.top/")
function(lrc_proxied_urls OUT_VAR)
  set(mirrored)
  foreach(u IN LISTS ARGN)
    if(LRC_PROXY AND u MATCHES "^https://github\\.com/")
      string(REPLACE "https://github.com/" "${LRC_PROXY}https://github.com/" mu "${u}")
      list(APPEND mirrored "${mu}")
    endif()
    list(APPEND mirrored "${u}")
  endforeach()
  set(${OUT_VAR} "${mirrored}" PARENT_SCOPE)
endfunction()

find_package(Threads REQUIRED)

# --- lensfun (optional profile database) ---------------------------------
# vcpkg exports the library as a CMake config package. Keep this optional so
# headless builds remain usable when the dependency is not installed yet.
set(LRC_LENSFUN_FOUND FALSE)
if(LRC_ENABLE_LENSFUN)
  find_package(lensfun CONFIG QUIET)
  if(TARGET lensfun::lensfun OR TARGET lensfun)
    set(LRC_LENSFUN_FOUND TRUE)
    message(STATUS "Lensfun: found (lens profiles enabled)")
  else()
    # The vcpkg lensfun port currently installs headers and a library but no
    # lensfunConfig.cmake. Discover that layout explicitly and expose the
    # same target name used by config-package installations.
    find_path(LENSFUN_INCLUDE_DIR NAMES lensfun/lensfun.h)
    find_library(LENSFUN_LIBRARY NAMES lensfun)
    if(LENSFUN_INCLUDE_DIR AND LENSFUN_LIBRARY)
      add_library(lensfun::lensfun UNKNOWN IMPORTED)
      set_target_properties(lensfun::lensfun PROPERTIES
        IMPORTED_LOCATION "${LENSFUN_LIBRARY}"
        IMPORTED_IMPLIB "${LENSFUN_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${LENSFUN_INCLUDE_DIR}")
      set(LRC_LENSFUN_FOUND TRUE)
      message(STATUS "Lensfun: found via headers/library (lens profiles enabled)")
    else()
      message(STATUS "Lensfun: NOT found — lens profile correction disabled. "
                     "Install the vcpkg lensfun:x64-windows port to enable it.")
    endif()
  endif()
endif()

# --- libraw ---------------------------------------------------------------
if(TARGET raw)
  message(STATUS "libraw: provided by caller")
elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/third_party/libraw/CMakeLists.txt")
  message(STATUS "libraw: vendored under third_party/libraw")
  add_subdirectory(third_party/libraw)
else()
  message(STATUS "libraw: FetchContent")
  lrc_proxied_urls(LIBRAW_URLS
    "https://www.libraw.org/data/LibRaw-0.22.0.tar.gz"
    "https://github.com/LibRaw/LibRaw/archive/refs/tags/0.22.0.tar.gz")
  FetchContent_Declare(libraw URL ${LIBRAW_URLS} DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  set(ENABLE_SHARED OFF CACHE BOOL "" FORCE)
  set(ENABLE_STATIC ON CACHE BOOL "" FORCE)
  set(ENABLE_RAWSPEED OFF CACHE BOOL "" FORCE)
  set(ENABLE_DCRAW_PROCESS ON CACHE BOOL "" FORCE)
  set(ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(ENABLE_OPENMP OFF CACHE BOOL "" FORCE)
  set(ENABLE_JPEG OFF CACHE BOOL "" FORCE)
  set(ENABLE_LCMS OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(libraw)
  # libraw's build interface may not propagate its src/ include dir; make the
  # header location explicit so `#include <libraw/libraw.h>` always works.
  if(TARGET raw AND EXISTS "${libraw_SOURCE_DIR}/src")
    target_include_directories(raw PUBLIC "${libraw_SOURCE_DIR}/src")
  endif()
endif()

# --- lcms2 -----------------------------------------------------------------
if(TARGET lcms2)
  message(STATUS "lcms2: provided by caller")
elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/third_party/lcms2/CMakeLists.txt")
  message(STATUS "lcms2: vendored under third_party/lcms2")
  add_subdirectory(third_party/lcms2)
else()
  message(STATUS "lcms2: FetchContent")
  lrc_proxied_urls(LCMS_URLS
    "https://github.com/mm2/Little-CMS/releases/download/lcms2.17/lcms2-2.17.zip"
    "https://downloads.sourceforge.net/project/lcms/lcms/2.17/lcms2-2.17.zip")
  FetchContent_Declare(lcms2 URL ${LCMS_URLS} DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  set(LCMS_BUILD_SHARED OFF CACHE BOOL "" FORCE)
  set(LCMS_DISABLE_TESTS ON CACHE BOOL "" FORCE)
  set(LCMS_DISABLE_ACCURATE_CACHE FALSE CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(lcms2)
endif()

# --- stb_image_write (vendored single-header, JPEG encode) -----------------
if(NOT TARGET stb_image_write)
  add_library(stb_image_write INTERFACE)
  target_include_directories(stb_image_write INTERFACE
    "${CMAKE_CURRENT_LIST_DIR}/third_party/stb")
endif()

# --- sqlite3 (amalgamation) ------------------------------------------------
if(TARGET sqlite3)
  message(STATUS "sqlite3: provided by caller")
elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/third_party/sqlite/sqlite3.c")
  message(STATUS "sqlite3: vendored under third_party/sqlite")
  add_library(sqlite3 STATIC third_party/sqlite/sqlite3.c)
  target_include_directories(sqlite3 PUBLIC third_party/sqlite)
else()
  message(STATUS "sqlite3: FetchContent")
  lrc_proxied_urls(SQLITE_URLS
    "https://www.sqlite.org/2025/sqlite-amalgamation-3490000.zip")
  FetchContent_Declare(sqlite3fc URL ${SQLITE_URLS} DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_GetProperties(sqlite3fc)
  if(NOT sqlite3fc_POPULATED)
    FetchContent_Populate(sqlite3fc)
  endif()
  add_library(sqlite3 STATIC "${sqlite3fc_SOURCE_DIR}/sqlite3.c")
  target_include_directories(sqlite3 PUBLIC "${sqlite3fc_SOURCE_DIR}")
endif()
target_compile_definitions(sqlite3 PUBLIC SQLITE_THREADSAFE=1 SQLITE_USE_URI=1)

# --- pugixml (vendored single-header-pair XML parser, XMP sidecars) ---------
if(NOT TARGET pugixml)
  add_library(pugixml STATIC third_party/pugixml/pugixml.cpp)
  target_include_directories(pugixml PUBLIC third_party/pugixml)
  if(MSVC)
    target_compile_options(pugixml PRIVATE /W4 /wd4100 /wd4244 /wd4996)
  endif()
endif()

# --- catch2 (tests only) ---------------------------------------------------
if(LRC_BUILD_TESTS)
  if(TARGET Catch2::Catch2)
    message(STATUS "catch2: provided by caller")
  elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/third_party/Catch2/CMakeLists.txt")
    message(STATUS "catch2: vendored under third_party/Catch2")
    set(CATCH_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(CATCH_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
    set(CATCH_INSTALL_EXTRAS OFF CACHE BOOL "" FORCE)
    add_subdirectory(third_party/Catch2)
  else()
    message(STATUS "catch2: FetchContent")
    lrc_proxied_urls(CATCH_URLS
      "https://github.com/catchorg/Catch2/archive/refs/tags/v3.7.1.tar.gz")
    FetchContent_Declare(catch2fc URL ${CATCH_URLS} DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    set(CATCH_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(catch2fc)
  endif()
endif()

# --- ONNX Runtime (AI, optional, engine-level) ----------------------------
# Attempt to find ONNX Runtime when AI is enabled. If not found, gracefully
# disable LRC_ENABLE_AI so the rest of the project still builds.
set(LRC_ONNXRUNTIME_FOUND FALSE)
if(LRC_ENABLE_AI)
  find_package(ONNXRuntime QUIET)
  if(ONNXRuntime_FOUND OR ONNXRUNTIME_FOUND)
    set(LRC_ONNXRUNTIME_FOUND TRUE)
    message(STATUS "ONNX Runtime: found (AI enabled)")
  else()
    # Also try the lowercase name that vcpkg sometimes exports
    find_package(onnxruntime QUIET)
    if(onnxruntime_FOUND)
      set(LRC_ONNXRUNTIME_FOUND TRUE)
      message(STATUS "ONNX Runtime: found via onnxruntime (AI enabled)")
    else()
      # Fallback: a prebuilt official release package (the github asset
      # onnxruntime-win-x64-<ver>.zip) unpacked to a dir with include/ and
      # lib/onnxruntime.{lib,dll}. Point -DONNXRUNTIME_ROOT=<that dir>.
      if(ONNXRUNTIME_ROOT)
        set(_ort_inc "${ONNXRUNTIME_ROOT}/include")
        set(_ort_lib "${ONNXRUNTIME_ROOT}/lib/onnxruntime.lib")
        if(EXISTS "${_ort_inc}" AND EXISTS "${_ort_lib}")
          if(NOT TARGET ONNXRuntime::onnxruntime)
            add_library(ONNXRuntime::onnxruntime UNKNOWN IMPORTED)
            set_target_properties(ONNXRuntime::onnxruntime PROPERTIES
              IMPORTED_LOCATION "${_ort_lib}"
              IMPORTED_IMPLIB "${_ort_lib}"
              INTERFACE_INCLUDE_DIRECTORIES "${_ort_inc}")
          endif()
          set(LRC_ONNXRUNTIME_FOUND TRUE)
          message(STATUS "ONNX Runtime: found via ONNXRUNTIME_ROOT="
                         "${ONNXRUNTIME_ROOT} (prebuilt package, AI enabled)")
        else()
          message(WARNING "ONNXRUNTIME_ROOT set but "
                  "${_ort_inc} / ${_ort_lib} not found — check the path "
                  "points at the unpacked release top dir (containing "
                  "include/ and lib/).")
        endif()
      endif()
      if(NOT LRC_ONNXRUNTIME_FOUND)
        message(STATUS "ONNX Runtime: NOT found — forcing LRC_ENABLE_AI=OFF. "
                       "Install via vcpkg (onnxruntime), set -DONNXRuntime_DIR, "
                       "or -DONNXRUNTIME_ROOT=<unpacked release dir>. "
                       "AI features (PHASE_11/12/13/14) will be stubbed out.")
        set(LRC_ENABLE_AI OFF CACHE BOOL "Enable AI features (ONNX Runtime)" FORCE)
      endif()
    endif()
  endif()
endif()

# --- Qt6 (GUI only) --------------------------------------------------------
if(LRC_ENABLE_QT)
  # CMake 4.x with Ninja+MSVC sometimes rejects Qt6's version file due to
  # CMAKE_SIZEOF_VOID_P not being set at find_package time.  Bypass the check.
  set(QT_NO_PACKAGE_VERSION_CHECK TRUE)
  find_package(Qt6 REQUIRED COMPONENTS Widgets Network Concurrent)
endif()

# ---------------------------------------------------------------------------
# Core libraries
# ---------------------------------------------------------------------------
set(LRC_ENGINE_SOURCES
  src/engine/ImageBuffer.cpp
  src/engine/Adjustments.cpp
  src/engine/ColorManager.cpp
  src/engine/ImageFormats.cpp
  src/engine/RawDecoder.cpp
  src/engine/LensfunProfile.cpp
  src/engine/DcpProfile.cpp
  src/engine/Pipeline.cpp
  src/engine/ExportEngine.cpp
  src/engine/ImageIO.cpp
  src/engine/HdrMerge.cpp
  src/engine/DngConverter.cpp
  src/engine/ZoneCurve.cpp
  src/engine/Xmp.cpp
  src/engine/XmpSidecar.cpp
  src/engine/PanoramaStitcher.cpp
  # --- PHASE_11: AI inference infrastructure (pure C++, Qt-free) ---------
  src/engine/AiInference.cpp
  # --- PHASE_12: AI denoise processor ------------------------------------
  src/engine/AiDenoiseProcessor.cpp
  # --- PHASE_13: AI mask generator ---------------------------------------
  src/engine/AiMaskGenerator.cpp
  # --- PHASE_15: Soft proofing (lcms2, no AI dependency) -----------------
  src/engine/SoftProofing.cpp
  # --- PHASE_14: DuplicateDetector (pure C++, lives in app/ per spec, but
  #            must build in headless tests too: no Qt includes in it) ----
  src/app/DuplicateDetector.cpp)
set(LRC_CATALOG_SOURCES
  src/catalog/CatalogDatabase.cpp
  src/catalog/ThumbnailCache.cpp
  src/catalog/FolderMonitor.cpp
  src/catalog/LibraryManager.cpp
  src/catalog/StandardPreview.cpp)
set(LRC_UTILS_SOURCES
  src/utils/MathUtils.cpp
  src/utils/Json.cpp
  src/utils/ProfileIO.cpp
  src/utils/XmpProfile.cpp
  src/utils/Parallel.cpp
  src/utils/Sha256.cpp)

# engine: pure C++, Qt-free core
add_library(lrc_engine STATIC ${LRC_ENGINE_SOURCES} ${LRC_UTILS_SOURCES})
target_include_directories(lrc_engine PUBLIC src)
target_link_libraries(lrc_engine PUBLIC raw lcms2 sqlite3 stb_image_write pugixml Threads::Threads)
# libraw pulls in <windows.h>, whose min/max macros clobber std::min/std::max.
target_compile_definitions(lrc_engine PUBLIC LIBRAW_NODLL=1 NOMINMAX)
if(LRC_LENSFUN_FOUND)
  target_compile_definitions(lrc_engine PUBLIC LRC_ENABLE_LENSFUN=1)
  if(TARGET lensfun::lensfun)
    target_link_libraries(lrc_engine PUBLIC lensfun::lensfun)
  else()
    target_link_libraries(lrc_engine PUBLIC lensfun)
  endif()
else()
  target_compile_definitions(lrc_engine PUBLIC LRC_ENABLE_LENSFUN=0)
endif()
# Path used by ModelManager.cpp to locate the shipped manifest when the
# executable runs from a build directory (rather than the installed layout).
target_compile_definitions(lrc_engine PRIVATE
    "LRC_PROJECT_MANIFEST_DIR=\"${CMAKE_CURRENT_LIST_DIR}\"")
# --- Optional AI (ONNX Runtime) ------------------------------------------
if(LRC_ENABLE_AI AND LRC_ONNXRUNTIME_FOUND)
  target_compile_definitions(lrc_engine PUBLIC LRC_ENABLE_AI=1)
  # Accept both CMake package-naming conventions for onnxruntime.
  if(TARGET ONNXRuntime::onnxruntime)
    target_link_libraries(lrc_engine PUBLIC ONNXRuntime::onnxruntime)
  elseif(TARGET ONNXRuntime::OnnxRuntime)
    target_link_libraries(lrc_engine PUBLIC ONNXRuntime::OnnxRuntime)
  elseif(TARGET onnxruntime::onnxruntime)
    target_link_libraries(lrc_engine PUBLIC onnxruntime::onnxruntime)
  elseif(TARGET onnxruntime)
    target_link_libraries(lrc_engine PUBLIC onnxruntime)
  endif()
else()
  target_compile_definitions(lrc_engine PUBLIC LRC_ENABLE_AI=0)
endif()

if(MSVC)
  # /utf-8: force source charset to UTF-8 so non-ASCII comments and Chinese
  # characters in headers compile correctly on GBK-codepage systems.
  target_compile_options(lrc_engine PRIVATE /utf-8 /W4 /wd4100 /wd4244 /wd4996)
  target_compile_options(lrc_engine PUBLIC /bigobj /utf-8)
else()
  target_compile_options(lrc_engine PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# catalog: pure C++, depends on engine
add_library(lrc_catalog STATIC ${LRC_CATALOG_SOURCES})
target_include_directories(lrc_catalog PUBLIC src)
target_link_libraries(lrc_catalog PUBLIC lrc_engine)

# ---------------------------------------------------------------------------
# Application (GUI)
# ---------------------------------------------------------------------------
if(LRC_ENABLE_QT)
  set(CMAKE_AUTOMOC ON)
  set(CMAKE_AUTORCC ON)
  set(CMAKE_AUTOUIC ON)
  set(LRC_UI_SOURCES
    src/app/Application.cpp
    # --- PHASE_11: ModelManager (uses Qt Network for downloads) ---------
    src/app/ModelManager.cpp
    src/ui/MainWindow.cpp
    src/ui/ImportDialog.cpp
    src/ui/ExportDialog.cpp
    src/ui/HdrMergeDialog.cpp
    src/ui/ImageViewport.cpp
    src/ui/DevelopPanel.cpp
    src/ui/HistogramWidget.cpp
    src/ui/FilmstripWidget.cpp
    src/ui/NavigatorWidget.cpp
    src/ui/CurveEditor.cpp
    src/ui/PanelSection.cpp
    src/ui/ColorWheelWidget.cpp
    src/ui/ImportWorker.cpp
    src/ui/PreviewWorker.cpp
    src/ui/SnapshotDialog.cpp
    src/ui/EditHistoryDialog.cpp
    src/ui/CollectionsDialog.cpp
    src/ui/SmartCollectionsDialog.cpp
    src/ui/PanoramaDialog.cpp
    src/ui/PanoramaWorker.cpp
    # --- PHASE_14 / 15: Dialogs -----------------------------------------
    src/ui/DuplicateResultsDialog.cpp
    src/ui/SoftProofingDialog.cpp
    src/ui/ModelManagementWidget.cpp)
  # WIN32 => GUI-subsystem executable: no console window on launch. The
  # Qt6::EntryPointPrivate (qtmain) target supplies the WinMain shim that
  # forwards to the app's own main().
  add_executable(lrc_lightroom WIN32 src/main.cpp ${LRC_UI_SOURCES}
                 resources/resources.qrc)
  if(WIN32)
    # Embed the application icon and version info into the .exe via a resource
    # script. The relative path in resources/app.rc is resolved against the
    # script's own directory; a copy is also staged into the build dir so the
    # icon is found whichever way rc.exe looks it up.
    # /utf-8 makes rc.exe read the .rc as UTF-8 so the (c) sign in the
    # LegalCopyright string compiles correctly (default codepage is GBK).
    set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} /utf-8")
    configure_file(resources/icons/app.ico
                   ${CMAKE_CURRENT_BINARY_DIR}/icons/app.ico COPYONLY)
    target_sources(lrc_lightroom PRIVATE resources/app.rc)
  endif()
  target_link_libraries(lrc_lightroom PRIVATE
    lrc_catalog lrc_engine
    Qt6::Widgets Qt6::Network Qt6::Concurrent Qt6::EntryPointPrivate
    Qt6::CorePrivate)  # for QZipReader (private API) — used by ModelManager to extract .onnx from .zip download_url (Qualcomm NAFNet)
  target_include_directories(lrc_lightroom PRIVATE src)
  add_custom_command(TARGET lrc_lightroom POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_LIST_DIR}/resources/Camera"
            "$<TARGET_FILE_DIR:lrc_lightroom>/resources/Camera"
    COMMENT "Copying camera DCP profiles")
  # Propagate LRC_ENABLE_AI to the GUI layer so UI buttons can be conditionally
  # compiled.  The engine target already sets the same define; we set it here
  # explicitly so the GUI layer does not accidentally diverge.
  if(LRC_ENABLE_AI)
    target_compile_definitions(lrc_lightroom PRIVATE LRC_ENABLE_AI=1)
  else()
    target_compile_definitions(lrc_lightroom PRIVATE LRC_ENABLE_AI=0)
  endif()
  # Propagate LRC_ENABLE_QT as a compile macro so ModelManager.cpp's
  # `#if LRC_ENABLE_QT` blocks (QNetworkAccessManager download + the QZipReader
  # extraction path) are actually compiled in the GUI build. Without this the
  # entire Qt download path is dead code (#if 0).
  target_compile_definitions(lrc_lightroom PRIVATE LRC_ENABLE_QT=1)
  # So ModelManager.cpp (compiled into lrc_lightroom) can find the shipped
  # manifest JSON at build time (same path as the engine target, defined above).
  target_compile_definitions(lrc_lightroom PRIVATE
      "LRC_PROJECT_MANIFEST_DIR=\"${CMAKE_CURRENT_LIST_DIR}\"")
  if(MSVC)
    target_compile_options(lrc_lightroom PRIVATE /utf-8 /W3)
    target_compile_definitions(lrc_lightroom PRIVATE _CRT_SECURE_NO_WARNINGS)
  endif()
  # User-facing executable name. The CMake target stays `lrc_lightroom`
  # internally, but the produced binary is SilverHalide.exe.
  set_target_properties(lrc_lightroom PROPERTIES OUTPUT_NAME "SilverHalide")

  # --- NAFNet end-to-end smoke test (GUI-only, NOT part of the headless
  # Catch2 suite). Compiles ModelManager.cpp (the only app/ source it needs)
  # so it can drive a real 410 MB ZIP download + QZipReader extraction + an
  # AiInferenceEngine inference pass — none of which the headless tests cover.
  if(LRC_BUILD_TESTS AND LRC_ENABLE_AI)
    add_executable(lrc_nafnet_smoke
      tests/nafnet_smoke.cpp
      src/app/ModelManager.cpp)
    target_link_libraries(lrc_nafnet_smoke PRIVATE
      lrc_catalog lrc_engine
      Qt6::Network Qt6::CorePrivate)
    target_include_directories(lrc_nafnet_smoke PRIVATE src)
    target_compile_definitions(lrc_nafnet_smoke PRIVATE
      LRC_ENABLE_QT=1
      "LRC_PROJECT_MANIFEST_DIR=\"${CMAKE_CURRENT_LIST_DIR}\"")
    if(MSVC)
      target_compile_options(lrc_nafnet_smoke PRIVATE /utf-8 /W3 /EHsc)
      target_compile_definitions(lrc_nafnet_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    set_target_properties(lrc_nafnet_smoke PROPERTIES
      OUTPUT_NAME "nafnet_smoke")
  endif()
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(LRC_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
