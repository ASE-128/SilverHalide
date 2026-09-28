# Engine/catalog unit tests. These do not require Qt, so the core pipeline can
# be validated in a headless build before the GUI is even compiled.

add_executable(lrc_tests
  TestMathUtils.cpp
  TestJson.cpp
  TestAdjustments.cpp
  TestAutoAdjust.cpp
  TestImageBuffer.cpp
  TestColorManager.cpp
  TestDngConverter.cpp
  TestPipeline.cpp
  TestCatalogDatabase.cpp
  TestThumbnailCache.cpp
  TestLibraryManager.cpp
  TestExportEngine.cpp
  TestRawDecoder.cpp
  TestLensfunProfile.cpp
  TestDcpProfile.cpp
  TestMasks.cpp
  TestMaskRaster.cpp
  TestHdrMerge.cpp
  TestZoneCurve.cpp
  TestProfileIO.cpp
  TestParallel.cpp
  TestUndoStack.cpp
  TestViewportMath.cpp
  TestImageIO.cpp
  test_standard_preview.cpp
  test_incremental_pipeline.cpp
  test_postcrop_vignette.cpp
  test_range_masks.cpp
  TestSpotRemoval.cpp
  TestPanoramaStitcher.cpp
  TestRenderScratch.cpp
  TestSha256.cpp
  # PHASE_11..15 — AI inference + feature tests.
  TestAiInference.cpp
  TestAiDenoise.cpp
  TestAiMask.cpp
  TestDuplicateDetection.cpp
  TestSoftProofing.cpp
  # Competitive-alignment implementation (DirectML / ICC / rename /
  # watermark / snapshots).
  TestCompetitiveP1.cpp
  # Linear-light RGB histogram (exact bin counts, no sampling).
  TestHistogram.cpp
  # Folder monitoring: event-driven watcher + polling fallback.
  TestFolderWatcher.cpp
  # Named develop-preset library (save/list/load/delete/export).
  TestPresetLibrary.cpp
  # CPU/GPU switchable spatial-filter backend (DirectCompute + fallback).
  TestSpatialBackend.cpp
  # Engine-layer headless acceptance: synthetic-50MP render
  # timing + process-memory sampling for the render/upscale/encode anchors.
  TestPerfAnchors.cpp
  # Offline AI: auto-develop suggestions + batch pregrade.
  TestAutoDevelop.cpp
  TestPregrade.cpp)

target_link_libraries(lrc_tests PRIVATE lrc_catalog Catch2::Catch2WithMain)
# TestPerfAnchors.cpp uses GetProcessMemoryInfo (process-memory sampling) on
# Windows. psapi is provided by the MSVC/Windows SDK; not needed elsewhere.
if(WIN32)
  target_link_libraries(lrc_tests PRIVATE psapi)
endif()
target_include_directories(lrc_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(lrc_tests PRIVATE
  "LRC_SOURCE_ROOT=\"${CMAKE_SOURCE_DIR}\"")

if(MSVC)
  # /utf-8: the tests include engine headers with non-ASCII comments; without
  # it MSVC reads them in the ANSI code page (see the AGENTS.md comment rule).
  target_compile_options(lrc_tests PRIVATE /utf-8 /W3 /EHsc)
endif()

add_test(NAME lrc_tests COMMAND lrc_tests)

# ---------------------------------------------------------------------------
# GUI-layer Qt tests (P1 acceptance).
#
# Built only when LRC_ENABLE_QT=ON. Compiles the real widget sources that the
# headless suite cannot (PreviewWorker + the import/export/snapshot dialogs)
# and drives them with QtTest: PreviewWorker::batchExport produces real JPEG /
# PNG / TIFF16 files on disk, and the dialogs are constructed + interrogated.
# Run: build_gui/tests/lrc_qt_tests.exe
# ---------------------------------------------------------------------------
if(LRC_ENABLE_QT)
  find_package(Qt6 REQUIRED COMPONENTS Test Widgets Concurrent Network)

  set(CMAKE_AUTOMOC ON)

  add_executable(lrc_qt_tests
    lrc_qt_tests.cpp
    ../src/ui/ColorWheelWidget.cpp
    ../src/ui/CurveEditor.cpp
    ../src/ui/DevelopPanel.cpp
    ../src/ui/HistogramWidget.cpp
    ../src/ui/ImportWorker.cpp
    ../src/ui/ImageViewport.cpp
    ../src/ui/Logging.cpp
    ../src/ui/PanelSection.cpp
    ../src/ui/PreviewWorker.cpp
    ../src/ui/ExportDialog.cpp
    ../src/ui/ImportDialog.cpp
    ../src/ui/SnapshotDialog.cpp
    ../src/ui/PresetDialog.cpp
    # Dialogs whose worker/main-window wiring is covered by the smoke tests:
    # the soft-proofing dialog (视图 → 软打样…) and the duplicate-results dialog
    # (照片 → 查找重复项…).
    ../src/ui/SoftProofingDialog.cpp
    ../src/ui/DuplicateResultsDialog.cpp
    # ModelManager: manifest parsing, SHA256 gating and the ZIP extractor
    # are driven directly (no network) by ModelManagerTest.
    ../src/app/ModelManager.cpp)
  target_link_libraries(lrc_qt_tests PRIVATE
    lrc_catalog lrc_engine
    Qt6::Test Qt6::Widgets Qt6::Concurrent Qt6::Network Qt6::CorePrivate)
  target_include_directories(lrc_qt_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_definitions(lrc_qt_tests PRIVATE
    LRC_ENABLE_QT=1
    "LRC_PROJECT_MANIFEST_DIR=\"${CMAKE_SOURCE_DIR}\"")
  if(MSVC)
    target_compile_options(lrc_qt_tests PRIVATE /utf-8 /W3 /EHsc)
    target_compile_definitions(lrc_qt_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
  endif()

  add_test(NAME lrc_qt_tests COMMAND lrc_qt_tests)
endif()
