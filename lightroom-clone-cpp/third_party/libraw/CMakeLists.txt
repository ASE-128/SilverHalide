# Minimal CMake build for vendored LibRaw 0.22.0 (static library).
# LibRaw's official release tarball ships autotools only (see README.cmake);
# this script mirrors the source list from Makefile.am and produces the
# `raw` target consumed by the top-level CMakeLists.
#
# Optional subsystems (RawSpeed, RawSpeed3, DNG SDK, OpenMP) are disabled —
# matching the ENABLE_* settings the top-level CMakeLists uses for the
# FetchContent fallback.
cmake_minimum_required(VERSION 3.25)

project(LibRaw CXX)

set(LIBRAW_SOURCES
  src/libraw_c_api.cpp
  src/libraw_datastream.cpp
  src/decoders/canon_600.cpp
  src/decoders/crx.cpp
  src/decoders/pana8.cpp
  src/decoders/decoders_dcraw.cpp
  src/decoders/sonycc.cpp
  src/decompressors/losslessjpeg.cpp
  src/decoders/decoders_libraw_dcrdefs.cpp
  src/decoders/olympus14.cpp
  src/decoders/decoders_libraw.cpp
  src/decoders/dng.cpp
  src/decoders/fp_dng.cpp
  src/decoders/fuji_compressed.cpp
  src/decoders/generic.cpp
  src/decoders/kodak_decoders.cpp
  src/decoders/load_mfbacks.cpp
  src/decoders/smal.cpp
  src/decoders/unpack_thumb.cpp
  src/decoders/unpack.cpp
  src/demosaic/aahd_demosaic.cpp
  src/demosaic/ahd_demosaic.cpp
  src/demosaic/dcb_demosaic.cpp
  src/demosaic/dht_demosaic.cpp
  src/demosaic/misc_demosaic.cpp
  src/demosaic/xtrans_demosaic.cpp
  src/metadata/adobepano.cpp
  src/metadata/canon.cpp
  src/metadata/ciff.cpp
  src/metadata/cr3_parser.cpp
  src/metadata/epson.cpp
  src/metadata/exif_gps.cpp
  src/metadata/fuji.cpp
  src/metadata/identify_tools.cpp
  src/metadata/identify.cpp
  src/metadata/kodak.cpp
  src/metadata/leica.cpp
  src/metadata/makernotes.cpp
  src/metadata/mediumformat.cpp
  src/metadata/minolta.cpp
  src/metadata/misc_parsers.cpp
  src/metadata/nikon.cpp
  src/metadata/normalize_model.cpp
  src/metadata/olympus.cpp
  src/metadata/hasselblad_model.cpp
  src/metadata/p1.cpp
  src/metadata/pentax.cpp
  src/metadata/samsung.cpp
  src/metadata/sony.cpp
  src/metadata/tiff.cpp
  src/postprocessing/aspect_ratio.cpp
  src/postprocessing/dcraw_process.cpp
  src/postprocessing/mem_image.cpp
  src/postprocessing/postprocessing_aux.cpp
  src/postprocessing/postprocessing_utils_dcrdefs.cpp
  src/postprocessing/postprocessing_utils.cpp
  src/preprocessing/ext_preprocess.cpp
  src/preprocessing/raw2image.cpp
  src/preprocessing/subtract_black.cpp
  src/tables/cameralist.cpp
  src/tables/colorconst.cpp
  src/tables/colordata.cpp
  src/tables/wblists.cpp
  src/utils/curves.cpp
  src/utils/decoder_info.cpp
  src/utils/init_close_utils.cpp
  src/utils/open.cpp
  src/utils/phaseone_processing.cpp
  src/utils/read_utils.cpp
  src/utils/thumb_utils.cpp
  src/utils/utils_dcraw.cpp
  src/utils/utils_libraw.cpp
  src/write/apply_profile.cpp
  src/write/file_write.cpp
  src/write/tiff_writer.cpp
  src/x3f/x3f_parse_process.cpp
  src/x3f/x3f_utils_patched.cpp
)

add_library(raw STATIC ${LIBRAW_SOURCES})
# Root dir exposes <libraw/libraw.h>; internal headers are reached via the
# relative "../../internal/..." includes the sources already use.
target_include_directories(raw PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(raw PUBLIC LIBRAW_NODLL=1)
target_compile_definitions(raw PRIVATE LIBRAW_BUILDLIB=1 _CRT_SECURE_NO_WARNINGS)

if(MSVC)
  target_compile_options(raw PRIVATE /W4 /wd4100 /wd4189 /wd4244 /wd4996 /bigobj)
else()
  target_compile_options(raw PRIVATE -w)
endif()
