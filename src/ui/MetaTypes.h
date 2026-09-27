#pragma once

/**
 * @brief Qt meta-type declarations for the engine types passed across queued
 *        signal/slot connections (worker threads -> UI thread). Register them in
 *        main() with qRegisterMetaType.
 *
 * @warning This header is the SINGLE declaration point for those types. Every
 *          Q_DECLARE_METATYPE expands to an explicit QMetaTypeId<T>
 *          specialization, so redeclaring one of these types anywhere else (a
 *          worker header, a test) makes any translation unit that includes both
 *          fail to compile with MSVC C2766. Include this header instead.
 */

#include <QMetaType>

#include "catalog/LibraryManager.h"
#include "engine/Adjustments.h"
#include "engine/ImageBuffer.h"
#include "engine/Pipeline.h"
#include "engine/SoftProofing.h"

Q_DECLARE_METATYPE(lrc::ImageBufferPtr)
Q_DECLARE_METATYPE(lrc::Adjustments)
Q_DECLARE_METATYPE(lrc::ImportMode)
// P3-1: linear-light RGB histogram bins cross the preview-worker queued
// connection (worker thread -> UI thread).
Q_DECLARE_METATYPE(lrc::HistogramBins)
// Soft-proofing parameters cross the UI -> preview-worker queued connection
// (the dialog's paramsChanged handler dispatches the proof render).
Q_DECLARE_METATYPE(lrc::SoftProofParams)
