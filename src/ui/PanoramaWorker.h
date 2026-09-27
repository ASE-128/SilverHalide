#pragma once

/**
 * @brief Background panorama-stitch worker.
 *
 * Owns a private copy of the ordered source buffers and the PanoramaOptions,
 * then runs lrc::stitchPanorama() on its own QThread (the dialog moves it onto
 * a QThread before invokeMethod("run")).
 *
 * Cancellation model: lrc::stitchPanorama is a single monolithic engine call
 * with no interrupt hook, so requestCancel() only takes effect at the two safe
 * points -- before the call starts, and after it returns. A pending cancel
 * discards the freshly stitched result (and any error) and reports cancelled()
 * instead of finished()/failed(). All result signals cross back to the GUI
 * thread as queued connections, so the worker never touches UI widgets.
 */

#include <atomic>
#include <vector>

#include <QObject>
#include <QString>

#include "engine/PanoramaStitcher.h"
// finished() carries lrc::ImageBufferPtr back to the GUI thread, so its metatype
// must be declared before moc's generated code uses it.
#include "ui/MetaTypes.h"

class PanoramaWorker : public QObject {
    Q_OBJECT
public:
    explicit PanoramaWorker(const std::vector<lrc::ImageBufferPtr>& sources,
                            const lrc::PanoramaOptions& opts,
                            QObject* parent = nullptr);

    /**
     * @brief Requests cancellation; only takes effect at the next safe point.
     *        Thread-safe.
     */
    void requestCancel() { cancel_.store(true); }

public slots:
    void run();

signals:
    /**
     * @brief Stitch completed successfully. result is the linear ProPhoto F32
     *        panorama. The applied PanoramaOptions stay internal to the worker;
     *        the dialog keeps its own copy (see PanoramaDialog::options()).
     */
    void finished(const lrc::ImageBufferPtr& result);
    /**
     * @brief The engine threw (e.g. no reliable overlap between photos).
     */
    void failed(const QString& message);
    /**
     * @brief A cancel was requested; no result is delivered.
     */
    void cancelled();

private:
    std::vector<lrc::ImageBufferPtr> sources_;
    lrc::PanoramaOptions opts_;
    std::atomic<bool> cancel_{false};
};
