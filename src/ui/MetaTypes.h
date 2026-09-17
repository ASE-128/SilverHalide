#pragma once

/**
 * @brief Qt meta-type declarations for the engine types passed across queued
 *        signal/slot connections (worker threads -> UI thread). Register them in
 *        main() with qRegisterMetaType.
 */

#include <QMetaType>

#include "catalog/LibraryManager.h"
#include "engine/Adjustments.h"
#include "engine/ImageBuffer.h"

Q_DECLARE_METATYPE(lrc::ImageBufferPtr)
Q_DECLARE_METATYPE(lrc::Adjustments)
Q_DECLARE_METATYPE(lrc::ImportMode)
