#include "ui/PanoramaDialog.h"
#include "ui/PanoramaWorker.h"
#include "ui/WheelGuard.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QMessageBox>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

namespace {

// Builds a small display thumbnail for the source strip. Sources
// may be linear U16/F32 or display-ready U8; linear formats get a simple sRGB
// gamma here (the dialog has no ColorManager -- the strip is only a preview and
// the stitch itself always works in linear ProPhoto inside the engine).
QImage makeThumbnail(const lrc::ImageBufferPtr& src, int maxEdge = 96) {
    if (!src || src->empty())
        return {};
    const int w = src->width();
    const int h = src->height();
    double scale = 1.0;
    if (w > h)
        scale = static_cast<double>(maxEdge) / static_cast<double>(w);
    else
        scale = static_cast<double>(maxEdge) / static_cast<double>(h);
    scale = std::min(scale, 1.0);
    const int tw = std::max(1, static_cast<int>(w * scale));
    const int th = std::max(1, static_cast<int>(h * scale));

    QImage out(tw, th, QImage::Format_RGB888);
    if (src->format() == lrc::PixelFormat::U8) {
        // Already display-ready sRGB bytes.
        const lrc::ImageBufferPtr small = src->resize(tw, th);
        if (!small)
            return {};
        const size_t stride = static_cast<size_t>(tw) * 3;
        for (int y = 0; y < th; ++y) {
            std::memcpy(out.scanLine(y),
                        small->data().data() + static_cast<size_t>(y) * stride,
                        stride);
        }
        return out;
    }

    // Linear U16/F32: area-average downscale to F32, then apply the sRGB gamma for
    // display.
    const lrc::ImageBufferPtr small = src->resizeToF32(tw, th);
    if (!small)
        return {};
    const float* pixels = reinterpret_cast<const float*>(small->data().data());
    for (int y = 0; y < th; ++y) {
        unsigned char* row = out.scanLine(y);
        const float* srcRow =
            pixels + static_cast<size_t>(y) * static_cast<size_t>(tw) * 3;
        for (int x = 0; x < tw; ++x) {
            const float* p = srcRow + static_cast<size_t>(x) * 3;
            for (int c = 0; c < 3; ++c) {
                float v = std::max(0.0f, std::min(1.0f, p[c]));
                // linear -> sRGB approximation (display preview only).
                v = (v <= 0.0031308f)
                        ? 12.92f * v
                        : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
                row[static_cast<size_t>(x) * 3 + c] =
                    static_cast<unsigned char>(v * 255.0f + 0.5f);
            }
        }
    }
    return out;
}

} // namespace

PanoramaDialog::PanoramaDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("全景合成"));
    setMinimumSize(580, 470);
    buildUi();
}

PanoramaDialog::~PanoramaDialog() {
    // Last resort: never destroy a running QThread with the dialog. Normal flow
    // already stopped the worker in accept()/reject()/the terminal slots; this
    // guards force-destruction edge cases. The stitch is monolithic, so the wait
    // may take a while.
    if (thread_ && thread_->isRunning()) {
        if (worker_)
            worker_->requestCancel();
        thread_->quit();
        thread_->wait();
    }
}

void PanoramaDialog::buildUi() {
    auto* root = new QVBoxLayout(this);
    root->setSpacing(8);

    hintLabel_ = new QLabel(
        tr("载入 2–20 张水平/垂直重叠的照片，选择投影方式后点击「开始合成」。"), this);
    hintLabel_->setWordWrap(true);
    root->addWidget(hintLabel_);

    // Thumbnail strip: horizontal scrolling row of numbered preview cells.
    thumbArea_ = new QScrollArea(this);
    thumbArea_->setWidgetResizable(true);
    thumbArea_->setFixedHeight(118);
    thumbArea_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    thumbArea_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* thumbHost = new QWidget;
    thumbLayout_ = new QHBoxLayout(thumbHost);
    thumbLayout_->setSpacing(6);
    thumbLayout_->setContentsMargins(6, 6, 6, 6);
    thumbLayout_->addStretch();
    thumbArea_->setWidget(thumbHost);
    root->addWidget(thumbArea_);

    auto* projRow = new QHBoxLayout;
    projRow->addWidget(new QLabel(tr("投影模式:"), this));
    projectionCombo_ = new QComboBox(this);
    projectionCombo_->addItem(tr("圆柱投影 (推荐)"), true);
    projectionCombo_->addItem(tr("纯平移拼贴"), false);
    projRow->addWidget(projectionCombo_, 1);
    root->addLayout(projRow);

    autoCropCheck_ = new QCheckBox(tr("自动裁剪空白边缘"), this);
    autoCropCheck_->setChecked(true);
    root->addWidget(autoCropCheck_);

    auto* focalRow = new QHBoxLayout;
    focalRow->addWidget(new QLabel(tr("焦距估算 (mm):"), this));
    focalSpin_ = new NoWheelDoubleSpinBox(this);
    focalSpin_->setRange(8.0, 200.0);
    focalSpin_->setDecimals(1);
    focalSpin_->setSingleStep(1.0);
    focalSpin_->setValue(35.0);
    focalSpin_->setSuffix(tr(" mm"));
    focalRow->addWidget(focalSpin_, 1);
    root->addLayout(focalRow);

    progressBar_ = new QProgressBar(this);
    progressBar_->setRange(0, 100);
    progressBar_->setValue(0);
    progressBar_->setVisible(false);
    root->addWidget(progressBar_);

    stageLabel_ = new QLabel(this);
    stageLabel_->setVisible(false);
    root->addWidget(stageLabel_);

    auto* btnRow = new QHBoxLayout;
    btnRow->addStretch();
    cancelButton_ = new QPushButton(tr("取消"), this);
    cancelButton_->setProperty("secondary", true);
    btnRow->addWidget(cancelButton_);
    startButton_ = new QPushButton(tr("开始合成"), this);
    startButton_->setEnabled(false);
    startButton_->setDefault(true);
    btnRow->addWidget(startButton_);
    root->addLayout(btnRow);

    stageTimer_ = new QTimer(this);
    stageTimer_->setInterval(2500);

    connect(startButton_, &QPushButton::clicked, this,
            &PanoramaDialog::onStartClicked);
    connect(cancelButton_, &QPushButton::clicked, this,
            &PanoramaDialog::onCancelClicked);
    connect(projectionCombo_,
            QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &PanoramaDialog::onProjectionChanged);
    connect(stageTimer_, &QTimer::timeout, this, &PanoramaDialog::onStageTimer);
}

void PanoramaDialog::setSources(
    const std::vector<lrc::ImageBufferPtr>& sources) {
    sources_ = sources;
    rebuildThumbnails();
    const int n = static_cast<int>(sources_.size());
    if (n == 0) {
        hintLabel_->setText(tr("没有可用的源照片。"));
    } else if (n < 2) {
        hintLabel_->setText(tr("需要至少 2 张照片（当前 %1 张）。").arg(n));
    } else if (n > 20) {
        hintLabel_->setText(tr("最多支持 20 张照片（当前 %1 张）。").arg(n));
    } else {
        hintLabel_->setText(
            tr("已载入 %1 张照片，点击「开始合成」进行全景拼接。").arg(n));
    }
    startButton_->setEnabled(n >= 2 && n <= 20);
}

void PanoramaDialog::rebuildThumbnails() {
    // Remove old thumbnail cells, keeping the trailing stretch at the last index.
    while (thumbLayout_->count() > 1) {
        QLayoutItem* item = thumbLayout_->takeAt(0);
        if (QWidget* w = item->widget())
            delete w;
        delete item;
    }
    for (size_t i = 0; i < sources_.size(); ++i) {
        auto* cell = new QWidget;
        auto* v = new QVBoxLayout(cell);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(2);
        auto* num = new QLabel(tr("第 %1 张").arg(static_cast<int>(i + 1)), cell);
        num->setAlignment(Qt::AlignCenter);
        v->addWidget(num);
        auto* img = new QLabel(cell);
        img->setFixedSize(100, 64);
        img->setAlignment(Qt::AlignCenter);
        const QImage thumb = makeThumbnail(sources_[i]);
        if (!thumb.isNull()) {
            img->setPixmap(QPixmap::fromImage(thumb).scaled(
                img->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
        } else {
            img->setText(tr("无预览"));
        }
        v->addWidget(img);
        thumbLayout_->insertWidget(thumbLayout_->count() - 1, cell);
    }
}

void PanoramaDialog::onProjectionChanged(int /*index*/) {
    // Focal length only matters for cylindrical mode.
    focalSpin_->setEnabled(projectionCombo_->currentData().toBool());
}

lrc::PanoramaOptions PanoramaDialog::currentOptions() const {
    lrc::PanoramaOptions o;
    o.focalLengthMm = static_cast<float>(focalSpin_->value());
    o.cylindrical = projectionCombo_->currentData().toBool();
    o.autoCrop = autoCropCheck_->isChecked();
    // alignMaxDim / bandRows 保留引擎默认 (keep the engine defaults).
    return o;
}

void PanoramaDialog::onStartClicked() {
    const int n = static_cast<int>(sources_.size());
    if (n < 2) {
        QMessageBox::information(this, tr("全景合成"), tr("请至少选择 2 张照片。"));
        return;
    }
    if (n > 20) {
        QMessageBox::information(this, tr("全景合成"), tr("最多支持 20 张照片。"));
        return;
    }
    // Guard: never start twice while a stitch runs.
    if (thread_ && thread_->isRunning())
        return;
    opts_ = currentOptions();
    setBusy(true);

    thread_ = new QThread(this);
    worker_ = new PanoramaWorker(sources_, opts_);
    worker_->moveToThread(thread_);
    connect(thread_, &QThread::finished, worker_, &QObject::deleteLater);
    connect(worker_, &PanoramaWorker::finished, this,
            &PanoramaDialog::onMergeFinished);
    connect(worker_, &PanoramaWorker::failed, this,
            &PanoramaDialog::onMergeFailed);
    connect(worker_, &PanoramaWorker::cancelled, this,
            &PanoramaDialog::onMergeCancelled);
    thread_->start();
    QMetaObject::invokeMethod(worker_, "run", Qt::QueuedConnection);
}

void PanoramaDialog::onCancelClicked() {
    if (thread_ && thread_->isRunning()) {
        // In-flight: request cancel; the worker reports cancelled() once the monolithic
        // stitch returns.
        if (worker_)
            worker_->requestCancel();
        cancelButton_->setEnabled(false);
        stageLabel_->setText(tr("正在取消… (请稍候)"));
        return;
    }
    reject();
}

void PanoramaDialog::reject() {
    if (thread_ && thread_->isRunning()) {
        // User tried to close while a stitch runs: request cancel, stay open, wait for
        // the worker's terminal signal to close us.
        if (worker_)
            worker_->requestCancel();
        cancelButton_->setEnabled(false);
        stageLabel_->setText(tr("正在取消… (请稍候)"));
        return;
    }
    emit cancelled();
    QDialog::reject();
}

void PanoramaDialog::onMergeFinished(const lrc::ImageBufferPtr& result) {
    stopThread();
    setBusy(false);
    // This slot runs on the same thread, so the emit synchronously finishes the
    // file write + catalog import before the dialog closes. opts_ holds the
    // options that were applied; callers read them via options().
    emit mergedReady(result);
    accept();
}

void PanoramaDialog::onMergeFailed(const QString& message) {
    stopThread();
    setBusy(false);
    QMessageBox::warning(this, tr("全景合成失败"), message);
    // Retryable: options and the start button are re-enabled, the sources stay
    // untouched.
}

void PanoramaDialog::onMergeCancelled() {
    stopThread();
    setBusy(false);
    emit cancelled();
    // Call the base reject directly to avoid the overridden reject() emitting
    // cancelled() twice.
    QDialog::reject();
}

void PanoramaDialog::onStageTimer() {
    // Coarse UI-side approximation: the engine's stitchPanorama is a single
    // monolithic call with no progress callback, so briefly show "aligning" and
    // then switch to "blending".
    stageLabel_->setText(tr("正在合成全景…"));
    stageTimer_->stop();
}

void PanoramaDialog::setBusy(bool busy) {
    const int n = static_cast<int>(sources_.size());
    projectionCombo_->setEnabled(!busy);
    autoCropCheck_->setEnabled(!busy);
    focalSpin_->setEnabled(!busy && projectionCombo_->currentData().toBool());
    startButton_->setEnabled(!busy && n >= 2 && n <= 20);
    cancelButton_->setEnabled(true);
    progressBar_->setVisible(busy);
    stageLabel_->setVisible(busy);
    if (busy) {
        progressBar_->setRange(0, 0); // 忙碌/不确定进度 (busy, indeterminate).
        stageLabel_->setText(tr("正在对齐照片…"));
        stageTimer_->start();
    } else {
        progressBar_->setRange(0, 100);
        progressBar_->setValue(100);
        stageTimer_->stop();
        stageLabel_->clear();
    }
}

void PanoramaDialog::stopThread() {
    if (!thread_)
        return;
    if (thread_->isRunning()) {
        if (worker_)
            worker_->requestCancel();
        thread_->quit();
        thread_->wait();
    }
    thread_->deleteLater();
    thread_ = nullptr;
    // worker_ is released via QThread::finished -> deleteLater; null it to avoid a
    // dangling reference.
    worker_ = nullptr;
}
