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
  TestXmpDocument.cpp
  TestXmpProfile.cpp
  TestXmpSidecar.cpp
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
  # P1 — competitive-alignment implementation (DirectML / ICC / rename /
  # watermark / snapshots).
  TestCompetitiveP1.cpp)

target_link_libraries(lrc_tests PRIVATE lrc_catalog Catch2::Catch2WithMain)
target_include_directories(lrc_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(lrc_tests PRIVATE
  "LRC_SOURCE_ROOT=\"${CMAKE_SOURCE_DIR}\"")

if(MSVC)
  target_compile_options(lrc_tests PRIVATE /W3 /EHsc)
endif()

add_test(NAME lrc_tests COMMAND lrc_tests)
