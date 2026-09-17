#include "ui/PanoramaWorker.h"

#include <exception>
#include <stdexcept>

PanoramaWorker::PanoramaWorker(const std::vector<lrc::ImageBufferPtr>& sources,
                               const lrc::PanoramaOptions& opts, QObject* parent)
    : QObject(parent), sources_(sources), opts_(opts) {
    // opts_ is used only for the engine call; the dialog keeps its own copy for the
    // caller, so no metatype registration for PanoramaOptions is needed.
}

void PanoramaWorker::run() {
    // Safe point 1: a cancel requested before the stitch started.
    if (cancel_.load()) {
        emit cancelled();
        return;
    }

    lrc::ImageBufferPtr result;
    QString error;
    try {
        result = lrc::stitchPanorama(sources_, opts_);
    } catch (const std::runtime_error& e) {
        // No reliable overlap; the engine message carries the NCC score and the failing
        // image index.
        error = tr("所选照片无可靠重叠，请重新选择或调整顺序 (%1)")
                    .arg(QString::fromStdString(e.what()));
    } catch (const std::invalid_argument& e) {
        error = tr("全景合成参数无效: %1").arg(QString::fromStdString(e.what()));
    } catch (const std::exception& e) {
        error = tr("全景合成失败: %1").arg(QString::fromStdString(e.what()));
    } catch (...) {
        error = tr("全景合成失败:未知错误");
    }

    // Safe point 2: a cancel requested while the monolithic stitch ran. The
    // engine call cannot be interrupted mid-flight, so both the result and any
    // error are discarded -- cancel wins.
    if (cancel_.load()) {
        emit cancelled();
        return;
    }
    if (!error.isEmpty()) {
        emit failed(error);
        return;
    }
    if (!result) {
        emit failed(tr("全景合成失败:引擎返回空结果"));
        return;
    }
    emit finished(result);
}
