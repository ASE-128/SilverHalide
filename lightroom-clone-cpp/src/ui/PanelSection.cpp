#include "ui/PanelSection.h"

#include <QPushButton>
#include <QVBoxLayout>

namespace {

const QString kArrowCollapsed = QStringLiteral("▶  ");
const QString kArrowExpanded = QStringLiteral("▼  ");

} // namespace

PanelSection::PanelSection(const QString& title, QWidget* parent)
    : QWidget(parent) {
    header_ = new QPushButton(this);
    header_->setObjectName(QStringLiteral("panelHeader"));
    header_->setCheckable(true);
    header_->setChecked(true);
    header_->setCursor(Qt::PointingHandCursor);

    body_ = new QWidget(this);
    bodyLayout_ = new QVBoxLayout(body_);
    bodyLayout_->setContentsMargins(2, 1, 2, 1);
    bodyLayout_->setSpacing(1);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addWidget(header_);
    root->addWidget(body_);

    setTitle(title);
    connect(header_, &QPushButton::clicked, this, [this](bool checked) {
        body_->setVisible(checked);
        header_->setText((checked ? kArrowExpanded : kArrowCollapsed) + title_);
        emit toggled(checked);
    });
}

void PanelSection::setTitle(const QString& title) {
    title_ = title;
    header_->setText((isExpanded() ? kArrowExpanded : kArrowCollapsed) + title_);
}

void PanelSection::addWidget(QWidget* widget) {
    bodyLayout_->addWidget(widget);
}

void PanelSection::addLayout(QLayout* layout) {
    bodyLayout_->addLayout(layout);
}

bool PanelSection::isExpanded() const { return header_->isChecked(); }

void PanelSection::setExpanded(bool expanded) {
    header_->setChecked(expanded);
    body_->setVisible(expanded);
    header_->setText((expanded ? kArrowExpanded : kArrowCollapsed) + title_);
}
