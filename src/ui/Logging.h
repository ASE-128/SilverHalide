#pragma once

/**
 * @file Logging.h
 * @brief Logging categories owned by the GUI layer.
 */

#include <QLoggingCategory>

/**
 * @brief 1:1 entry instrumentation ("[PERF1A]" lines) for the P-A anchors.
 *
 * Off by default: a normal run must not pay for or drown in diagnostics. Enable
 * it per run with the standard Qt rule, e.g.
 *   QT_LOGGING_RULES="lrc.perf.info=true" build_gui/SilverHalide.exe
 * (main() only installs the default "off" rule when the environment variable is
 * empty, so an explicit rule always wins.)
 */
Q_DECLARE_LOGGING_CATEGORY(lrcPerf)

