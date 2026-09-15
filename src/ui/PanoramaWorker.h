#pragma once

// Background panorama-stitch worker. Owns a private copy of the ordered source
// buffers and the PanoramaOptions, then runs lrc::stitchPanorama() on its own
// QThread (the dialog moves it onto a QThread before invokeMethod("run")).
//
// Cancellation model: lrc::stitchPanorama is a single monolithic engine call
// with no interrupt hook, so requestCancel() only takes effect at the two safe
// points -- before the call starts, and after it returns. A pending cancel
// discards the freshly stitched result (and any error) and reports cancelled()
// instead of finished()/failed(). All result signals cross back to the GUI
// thread as queued connections, so the worker never touches UI widgets.

#include <atomic>
#include <vector>

#include <QObject>
#include <QString>

#include "engine/PanoramaStitcher.h"

class PanoramaWorker : public QObject {
    Q_OBJECT
public:
    explicit PanoramaWorker(const std::vector<lrc::ImageBufferPtr>& sources,
                            const lrc::PanoramaOptions& opts,
                            QObject* parent = nullptr);

    // 请求取消:仅在下一次安全点生效 (request cancellation; only takes effect at
    // the next safe point). Thread-safe.
    void requestCancel() { cancel_.store(true); }

public slots:
    void run();

signals:
    // Stitch completed successfully. `result` is the linear ProPhoto F32
    // panorama. The applied PanoramaOptions stay internal to the worker; the
    // dialog keeps its own copy (see PanoramaDialog::options()).
    void finished(const lrc::ImageBufferPtr& result);
    // The engine threw (e.g. no reliable overlap between adjacent photos).
    void failed(const QString& message);
    // A cancel was requested; no result is delivered.
    void cancelled();

private:
    std::vector<lrc::ImageBufferPtr> sources_;
    lrc::PanoramaOptions opts_;
    std::atomic<bool> cancel_{false};
};
