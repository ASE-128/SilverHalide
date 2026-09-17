#pragma once

/**
 * @brief Lightroom-style collapsible panel.
 *
 * A clickable full-width header toggles the content below it. Sections remember
 * their expanded state via the button's checked property (exposed for
 * MainWindow to persist). The header is styled through the `panelHeader`
 * object name in dark.qss.
 */

#include <QString>
#include <QWidget>

class QPushButton;
class QVBoxLayout;

class PanelSection : public QWidget {
    Q_OBJECT
public:
    explicit PanelSection(const QString& title, QWidget* parent = nullptr);

    // Adds a widget to the section body (takes ownership in the layout).
    void addWidget(QWidget* widget);
    void addLayout(QLayout* layout);

    QVBoxLayout* bodyLayout() const { return bodyLayout_; }
    QString title() const { return title_; }

    bool isExpanded() const;
    void setExpanded(bool expanded);

signals:
    void toggled(bool expanded);

private:
    void setTitle(const QString& title);

    QPushButton* header_ = nullptr;
    QWidget* body_ = nullptr;
    QVBoxLayout* bodyLayout_ = nullptr;
    QString title_;
};
