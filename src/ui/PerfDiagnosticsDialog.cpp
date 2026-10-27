#include "ui/PerfDiagnosticsDialog.h"

#include <utility>

#include <QFormLayout>
#include <QLabel>
#include <QTimer>
#include <QVBoxLayout>

namespace lrc {

namespace {

QString formatBytes(std::uint64_t bytes) {
    constexpr double kMB = 1024.0 * 1024.0;
    constexpr double kGB = 1024.0 * kMB;
    if (bytes == 0)
        return QStringLiteral("—");
    if (bytes >= static_cast<std::uint64_t>(kGB))
        return QStringLiteral("%1 GB").arg(bytes / kGB, 0, 'f', 2);
    return QStringLiteral("%1 MB")
        .arg(bytes / kMB, 0, 'f', static_cast<int>(bytes >= 10 * kMB ? 0 : 1));
}

QString formatMs(double ms) {
    return QStringLiteral("%1 ms").arg(ms, 0, 'f', 1);
}

}  // namespace

PerfDiagnosticsDialog::PerfDiagnosticsDialog(SnapshotProvider provider,
                                             QWidget* parent)
    : QDialog(parent), provider_(std::move(provider)) {
    setWindowTitle(tr("性能诊断"));
    setModal(false);
    setAttribute(Qt::WA_DeleteOnClose, false);
    resize(420, 240);

    auto* form = new QFormLayout();
    backendLabel_ = new QLabel(QStringLiteral("—"), this);
    timingLabel_ = new QLabel(QStringLiteral("—"), this);
    cacheLabel_ = new QLabel(QStringLiteral("—"), this);
    memoryLabel_ = new QLabel(QStringLiteral("—"), this);
    backendLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    timingLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    cacheLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    memoryLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);

    form->addRow(tr("像素后端"), backendLabel_);
    form->addRow(tr("精帧耗时"), timingLabel_);
    form->addRow(tr("预览缓存"), cacheLabel_);
    form->addRow(tr("进程内存"), memoryLabel_);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addStretch(1);

    // Pure polling: the dialog subscribes to no render signal, so high-frequency
    // preview traffic cannot drive repaints here.
    timer_ = new QTimer(this);
    timer_->setInterval(500);
    connect(timer_, &QTimer::timeout, this, &PerfDiagnosticsDialog::refreshNow);
    timer_->start();

    refreshNow();
}

void PerfDiagnosticsDialog::refreshNow() {
    const PerfDiagnosticsSnapshot snap = provider_ ? provider_()
                                                   : PerfDiagnosticsSnapshot{};
    backendLabel_->setText(
        tr("%1（偏好：%2）").arg(snap.backend, snap.preference));
    timingLabel_->setText(
        tr("中位 %1 · 最小 %2 · 最大 %3 · 样本 %4")
            .arg(formatMs(snap.medianMs), formatMs(snap.minMs),
                 formatMs(snap.maxMs))
            .arg(snap.sampleCount));
    cacheLabel_->setText(
        tr("命中 %1 · 未命中 %2 · 命中率 %3% · 占用 %4 / %5")
            .arg(snap.cacheHits)
            .arg(snap.cacheMisses)
            .arg(snap.cacheHitRate * 100.0, 0, 'f', 1)
            .arg(formatBytes(snap.cacheBytes), formatBytes(snap.cacheCapacityBytes)));
    memoryLabel_->setText(
        tr("当前 %1 · 峰值 %2")
            .arg(formatBytes(snap.workingSetBytes),
                 formatBytes(snap.peakWorkingSetBytes)));
}

}  // namespace lrc
