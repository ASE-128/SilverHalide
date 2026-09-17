#include "ui/SmartCollectionsDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStringList>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

#include "utils/Json.h"

// This translation unit is not inside `namespace lrc` (the dialog class lives at
// global scope), so alias the JSON namespace for the shorthand used below.
namespace json = lrc::json;

namespace {

// Rule builder metadata. `id` is the JSON field name; `label` is the Chinese
// display text shown in the field dropdown.
const struct { const char* id; const char* label; } kRuleFields[] = {
    {"rating", "评分"},
    {"label", "色标"},
    {"flag", "旗标"},
    {"keyword", "关键字"},
    {"date_taken", "拍摄日期"},
    {"file_type", "文件类型"},
    {"has_adjustments", "已调整"},
    {"camera", "相机"},
    {"lens", "镜头"},
    {"folder_path", "文件夹"},
    {"title", "标题"},
    {"caption", "说明"},
};

const char* const kOpsNumeric[] = {"==", "!=", "<",     "<=",
                                   ">",  ">=", "in",    "between"};
const char* const kOpsString[] = {"==",     "!=", "contains", "starts_with",
                                  "<",      "<=", ">",        ">=",
                                  "in",     "between"};
const char* const kOpsKeyword[] = {"==", "!=", "contains", "starts_with", "in"};
const char* const kOpsBool[] = {"==", "!="};

const char* const kFlagNames[] = {"已拒绝", "无旗标", "已选取"};
const int kFlagValues[] = {-1, 0, 1};

const char* const kLabelNames[] = {"无", "红", "橙", "黄", "绿", "青", "蓝", "紫"};
const char* const kLabelValues[] = {"none",   "red",   "orange", "yellow",
                                    "green",  "cyan",  "blue",   "purple"};

const char* const kFileTypes[] = {"dng", "arw",  "cr2", "nef", "rw2",
                                  "jpg", "jpeg", "png", "tif", "tiff"};
const char* const kBoolNames[] = {"是", "否"};

bool isNumericField(const QString& field) {
    return field == QStringLiteral("rating") ||
           field == QStringLiteral("flag") ||
           field == QStringLiteral("label");
}

void fillOps(QComboBox* combo, const char* const* ops, int count) {
    for (int i = 0; i < count; ++i)
        combo->addItem(QString::fromLatin1(ops[i]));
}

} // namespace

// One editable rule row in the builder: field combo + operator combo + value
// editor. The value editor switches between a line edit, an enum combo and a
// rating spin box depending on the selected field and operator.
//
// Defined at global scope (NOT in the anonymous namespace) to match the forward
// declaration in SmartCollectionsDialog.h -- an anonymous-namespace definition
// would be a distinct entity and make `RuleRowWidget` ambiguous (C2872).
class RuleRowWidget : public QWidget {
public:
    explicit RuleRowWidget(QWidget* parent = nullptr) : QWidget(parent) {
        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(4);

        fieldCombo_ = new QComboBox(this);
        for (const auto& f : kRuleFields)
            fieldCombo_->addItem(QString::fromUtf8(f.label),
                                 QString::fromLatin1(f.id));
        layout->addWidget(fieldCombo_, 1);

        opCombo_ = new QComboBox(this);
        layout->addWidget(opCombo_, 0);

        valueStack_ = new QStackedWidget(this);
        lineEdit_ = new QLineEdit(this);
        lineEdit_->setPlaceholderText(QStringLiteral("值"));
        enumCombo_ = new QComboBox(this);
        spinBox_ = new QSpinBox(this);
        spinBox_->setRange(0, 5);
        valueStack_->addWidget(lineEdit_);
        valueStack_->addWidget(enumCombo_);
        valueStack_->addWidget(spinBox_);
        layout->addWidget(valueStack_, 1);

        removeButton_ = new QPushButton(QStringLiteral("✕"), this);
        removeButton_->setFixedWidth(26);
        removeButton_->setToolTip(QStringLiteral("删除该规则"));
        layout->addWidget(removeButton_);

        connect(fieldCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this](int) { rebuildValueWidget(nullptr); });
        connect(opCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this](int) { rebuildValueWidget(nullptr); });

        rebuildValueWidget(nullptr);
    }

    QString field() const { return fieldCombo_->currentData().toString(); }
    QString op() const { return opCombo_->currentText(); }
    QPushButton* removeButton() const { return removeButton_; }

    // Serializes the row's value into the JSON rule value. `in` / `between`
    // parse the line edit as a comma-separated list into a JSON array.
    json::Value value() const {
        const QString opStr = op();
        if (opStr == QStringLiteral("in") ||
            opStr == QStringLiteral("between")) {
            json::Value arr = json::Value::array();
            const QStringList parts =
                lineEdit_->text().split(QLatin1Char(','), Qt::SkipEmptyParts);
            for (const QString& part : parts) {
                const QString trimmed = part.trimmed();
                if (trimmed.isEmpty())
                    continue;
                if (isNumericField(field())) {
                    bool ok = false;
                    const double d = trimmed.toDouble(&ok);
                    if (ok)
                        arr.push_back(json::Value(d));
                    else
                        arr.push_back(json::Value(trimmed.toStdString()));
                } else {
                    arr.push_back(json::Value(trimmed.toStdString()));
                }
            }
            return arr;
        }
        switch (enumKind_) {
        case 1:
            return json::Value(kFlagValues[enumCombo_->currentIndex()]);
        case 2:
            return json::Value(kLabelValues[enumCombo_->currentIndex()]);
        case 3:
            return json::Value(
                enumCombo_->currentText().trimmed().toLower().toStdString());
        case 4:
            return json::Value(enumCombo_->currentIndex() == 0);
        default: {
            const QString text = lineEdit_->text().trimmed();
            if (isNumericField(field())) {
                bool ok = false;
                const double d = text.toDouble(&ok);
                if (ok)
                    return json::Value(d);
            }
            return json::Value(text.toStdString());
        }
        }
    }

    void setRule(const QString& fieldId, const QString& opStr,
                 const json::Value& value) {
        for (int i = 0; i < fieldCombo_->count(); ++i) {
            if (fieldCombo_->itemData(i).toString() == fieldId) {
                fieldCombo_->setCurrentIndex(i);
                break;
            }
        }
        const int opIdx = opCombo_->findText(opStr);
        if (opIdx >= 0)
            opCombo_->setCurrentIndex(opIdx);
        rebuildValueWidget(&value);
    }

private:
    void rebuildValueWidget(const json::Value* initial) {
        const QString f = field();
        const QString o = op();
        const bool listOp = (o == QStringLiteral("in") ||
                             o == QStringLiteral("between"));
        enumKind_ = 0;

        if (f == QStringLiteral("has_adjustments") && !listOp) {
            enumKind_ = 4;
            if (enumCombo_->itemData(0).toString() != QStringLiteral("bool")) {
                enumCombo_->clear();
                enumCombo_->addItem(QString::fromUtf8(kBoolNames[0]),
                                    QStringLiteral("bool"));
                enumCombo_->addItem(QString::fromUtf8(kBoolNames[1]),
                                    QStringLiteral("bool"));
                enumCombo_->setEditable(false);
            }
            valueStack_->setCurrentWidget(enumCombo_);
        } else if (f == QStringLiteral("rating") && !listOp) {
            valueStack_->setCurrentWidget(spinBox_);
        } else if (f == QStringLiteral("flag") && !listOp) {
            enumKind_ = 1;
            if (enumCombo_->itemData(0).toString() != QStringLiteral("flag")) {
                enumCombo_->clear();
                for (int i = 0; i < 3; ++i)
                    enumCombo_->addItem(QString::fromUtf8(kFlagNames[i]),
                                        QStringLiteral("flag"));
                enumCombo_->setEditable(false);
            }
            valueStack_->setCurrentWidget(enumCombo_);
        } else if (f == QStringLiteral("label") && !listOp) {
            enumKind_ = 2;
            if (enumCombo_->itemData(0).toString() != QStringLiteral("label")) {
                enumCombo_->clear();
                for (int i = 0; i < 8; ++i)
                    enumCombo_->addItem(QString::fromUtf8(kLabelNames[i]),
                                        QStringLiteral("label"));
                enumCombo_->setEditable(false);
            }
            valueStack_->setCurrentWidget(enumCombo_);
        } else if (f == QStringLiteral("file_type") && !listOp) {
            enumKind_ = 3;
            if (enumCombo_->itemData(0).toString() !=
                QStringLiteral("file_type")) {
                enumCombo_->clear();
                for (const char* t : kFileTypes)
                    enumCombo_->addItem(QString::fromLatin1(t),
                                        QStringLiteral("file_type"));
                enumCombo_->setEditable(true);
            }
            valueStack_->setCurrentWidget(enumCombo_);
        } else {
            valueStack_->setCurrentWidget(lineEdit_);
        }

        // Refill the operator list for the current field kind.
        opCombo_->blockSignals(true);
        opCombo_->clear();
        if (f == QStringLiteral("has_adjustments"))
            fillOps(opCombo_, kOpsBool, 2);
        else if (f == QStringLiteral("keyword"))
            fillOps(opCombo_, kOpsKeyword, 5);
        else if (isNumericField(f))
            fillOps(opCombo_, kOpsNumeric, 8);
        else
            fillOps(opCombo_, kOpsString, 10);
        const int wantOp = opCombo_->findText(o);
        if (wantOp >= 0)
            opCombo_->setCurrentIndex(wantOp);
        opCombo_->blockSignals(false);

        if (initial != nullptr) {
            const json::Value& v = *initial;
            if (listOp) {
                QStringList parts;
                if (v.isArray()) {
                    for (const auto& item : v.items()) {
                        if (item.isString())
                            parts << QString::fromStdString(item.asString());
                        else if (item.isNumber())
                            parts << QString::number(item.asNumber());
                        else
                            parts << QString::fromStdString(item.serialize());
                    }
                } else if (v.isString()) {
                    parts << QString::fromStdString(v.asString());
                }
                lineEdit_->setText(parts.join(QLatin1Char(',')));
            } else {
                switch (enumKind_) {
                case 1: {
                    const int val = v.asInt(-999);
                    for (int i = 0; i < 3; ++i)
                        if (kFlagValues[i] == val) {
                            enumCombo_->setCurrentIndex(i);
                            break;
                        }
                    break;
                }
                case 2: {
                    const QString s = QString::fromStdString(v.asString());
                    for (int i = 0; i < 8; ++i)
                        if (QString::fromUtf8(kLabelValues[i]) == s) {
                            enumCombo_->setCurrentIndex(i);
                            break;
                        }
                    break;
                }
                case 3: {
                    const QString s = QString::fromStdString(v.asString());
                    const int idx =
                        enumCombo_->findText(s, Qt::MatchFixedString);
                    if (idx >= 0)
                        enumCombo_->setCurrentIndex(idx);
                    else
                        enumCombo_->setEditText(s);
                    break;
                }
                case 4:
                    enumCombo_->setCurrentIndex(v.asBool(false) ? 0 : 1);
                    break;
                default:
                    if (f == QStringLiteral("rating") && v.isNumber()) {
                        spinBox_->setValue(v.asInt(0));
                    } else if (v.isString()) {
                        lineEdit_->setText(QString::fromStdString(v.asString()));
                    } else if (v.isNumber()) {
                        lineEdit_->setText(QString::number(v.asNumber()));
                    }
                    break;
                }
            }
        } else if (valueStack_->currentWidget() == spinBox_) {
            spinBox_->setValue(4);
        } else if (enumKind_ != 0) {
            enumCombo_->setCurrentIndex(0);
        } else {
            lineEdit_->clear();
        }
    }

    QComboBox* fieldCombo_ = nullptr;
    QComboBox* opCombo_ = nullptr;
    QStackedWidget* valueStack_ = nullptr;
    QLineEdit* lineEdit_ = nullptr;
    QComboBox* enumCombo_ = nullptr;
    QSpinBox* spinBox_ = nullptr;
    QPushButton* removeButton_ = nullptr;
    int enumKind_ = 0;
};

SmartCollectionsDialog::SmartCollectionsDialog(lrc::CatalogDatabase* catalog,
                                               QWidget* parent)
    : QDialog(parent), catalog_(catalog) {
    setWindowTitle(tr("管理智能收藏夹"));
    setMinimumSize(760, 480);

    auto* root = new QHBoxLayout(this);
    root->setSpacing(8);

    // Left column: smart-collection list + management buttons.
    auto* left = new QVBoxLayout;
    left->setSpacing(4);
    auto* leftCaption = new QLabel(tr("智能收藏夹"), this);
    leftCaption->setProperty("caption", true);
    left->addWidget(leftCaption);

    list_ = new QListWidget(this);
    left->addWidget(list_, 1);

    auto* newButton = new QPushButton(tr("新建"), this);
    editButton_ = new QPushButton(tr("编辑"), this);
    deleteButton_ = new QPushButton(tr("删除"), this);
    auto* leftButtons = new QHBoxLayout;
    leftButtons->addWidget(newButton);
    leftButtons->addWidget(editButton_);
    leftButtons->addWidget(deleteButton_);
    left->addLayout(leftButtons);
    root->addLayout(left, 1);

    // Right column: rule-builder editor.
    auto* right = new QVBoxLayout;
    right->setSpacing(6);

    auto* nameRow = new QHBoxLayout;
    nameRow->addWidget(new QLabel(tr("名称："), this));
    nameEdit_ = new QLineEdit(this);
    nameEdit_->setPlaceholderText(tr("智能收藏夹名称"));
    nameRow->addWidget(nameEdit_, 1);
    right->addLayout(nameRow);

    auto* matchRow = new QHBoxLayout;
    matchRow->addWidget(new QLabel(tr("匹配："), this));
    matchCombo_ = new QComboBox(this);
    matchCombo_->addItem(tr("满足所有规则 (ALL)"), QStringLiteral("ALL"));
    matchCombo_->addItem(tr("满足任一规则 (ANY)"), QStringLiteral("ANY"));
    matchRow->addWidget(matchCombo_, 1);
    right->addLayout(matchRow);

    right->addWidget(new QLabel(tr("规则："), this));

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    rulesContainer_ = new QWidget(this);
    rulesLayout_ = new QVBoxLayout(rulesContainer_);
    rulesLayout_->setContentsMargins(0, 0, 0, 0);
    rulesLayout_->setSpacing(4);
    scroll->setWidget(rulesContainer_);
    right->addWidget(scroll, 1);

    auto* ruleButtons = new QHBoxLayout;
    auto* addRuleButton = new QPushButton(tr("添加规则"), this);
    auto* removeRuleButton = new QPushButton(tr("删除规则"), this);
    ruleButtons->addWidget(addRuleButton);
    ruleButtons->addWidget(removeRuleButton);
    ruleButtons->addStretch(1);
    right->addLayout(ruleButtons);

    auto* buttonBox = new QDialogButtonBox(QDialogButtonBox::Save |
                                               QDialogButtonBox::Cancel,
                                           Qt::Horizontal, this);
    buttonBox->button(QDialogButtonBox::Save)->setText(tr("保存"));
    buttonBox->button(QDialogButtonBox::Cancel)->setText(tr("取消"));
    right->addWidget(buttonBox);
    root->addLayout(right, 2);

    refreshList();

    connect(list_, &QListWidget::itemSelectionChanged, this,
            &SmartCollectionsDialog::onSelectionChanged);
    connect(newButton, &QPushButton::clicked, this,
            &SmartCollectionsDialog::onNewCollection);
    connect(editButton_, &QPushButton::clicked, this,
            &SmartCollectionsDialog::onEditCollection);
    connect(deleteButton_, &QPushButton::clicked, this,
            &SmartCollectionsDialog::onDeleteCollection);
    connect(addRuleButton, &QPushButton::clicked, this,
            &SmartCollectionsDialog::onAddRule);
    connect(removeRuleButton, &QPushButton::clicked, this,
            &SmartCollectionsDialog::onRemoveRule);
    connect(buttonBox, &QDialogButtonBox::accepted, this,
            &SmartCollectionsDialog::onSave);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void SmartCollectionsDialog::refreshList() {
    const QSignalBlocker blocker(list_);
    list_->clear();
    const auto collections = catalog_->listSmartCollections();
    for (const lrc::SmartCollection& sc : collections) {
        auto* item = new QListWidgetItem(QString::fromStdString(sc.name), list_);
        item->setData(Qt::UserRole, QVariant::fromValue(sc.id));
    }
    if (collections.empty()) {
        auto* empty = new QListWidgetItem(tr("（无智能收藏夹）"), list_);
        empty->setFlags(Qt::NoItemFlags);
    } else if (list_->currentItem() == nullptr) {
        list_->setCurrentRow(0);
    }
    onSelectionChanged();
}

int64_t SmartCollectionsDialog::currentCollectionId() const {
    QListWidgetItem* item = list_->currentItem();
    if (!item)
        return -1;
    return item->data(Qt::UserRole).toLongLong();
}

QString SmartCollectionsDialog::collectionName(int64_t id) const {
    for (const lrc::SmartCollection& sc : catalog_->listSmartCollections()) {
        if (sc.id == id)
            return QString::fromStdString(sc.name);
    }
    return QString();
}

void SmartCollectionsDialog::onSelectionChanged() {
    const int64_t id = currentCollectionId();
    if (id <= 0) {
        editButton_->setEnabled(false);
        deleteButton_->setEnabled(false);
        return;
    }
    for (const lrc::SmartCollection& sc : catalog_->listSmartCollections()) {
        if (sc.id == id) {
            loadCollection(sc);
            editButton_->setEnabled(true);
            deleteButton_->setEnabled(true);
            return;
        }
    }
    editButton_->setEnabled(false);
    deleteButton_->setEnabled(false);
}

void SmartCollectionsDialog::clearEditor() {
    for (RuleRowWidget* row : rules_) {
        rulesLayout_->removeWidget(row);
        delete row;
    }
    rules_.clear();
    nameEdit_->clear();
    matchCombo_->setCurrentIndex(0);
    editingId_ = -1;
    auto* row = new RuleRowWidget(rulesContainer_);
    connect(row->removeButton(), &QPushButton::clicked, this,
            [this, row]() { removeRule(row); });
    rulesLayout_->addWidget(row);
    rules_.push_back(row);
    list_->clearSelection();
}

void SmartCollectionsDialog::loadCollection(const lrc::SmartCollection& sc) {
    for (RuleRowWidget* row : rules_) {
        rulesLayout_->removeWidget(row);
        delete row;
    }
    rules_.clear();

    nameEdit_->setText(QString::fromStdString(sc.name));
    matchCombo_->setCurrentIndex(
        sc.matchType == QStringLiteral("ANY") ? 1 : 0);
    editingId_ = sc.id;

    auto pred = json::Value::parse(sc.predicateJson);
    if (pred && pred->isObject() && (*pred)["rules"].isArray()) {
        for (const auto& rule : (*pred)["rules"].items()) {
            auto* row = new RuleRowWidget(rulesContainer_);
            connect(row->removeButton(), &QPushButton::clicked, this,
                    [this, row]() { removeRule(row); });
            row->setRule(QString::fromStdString(rule["field"].asString()),
                         QString::fromStdString(rule["op"].asString()),
                         rule["value"]);
            rulesLayout_->addWidget(row);
            rules_.push_back(row);
        }
    }
    if (rules_.empty()) {
        auto* row = new RuleRowWidget(rulesContainer_);
        connect(row->removeButton(), &QPushButton::clicked, this,
                [this, row]() { removeRule(row); });
        rulesLayout_->addWidget(row);
        rules_.push_back(row);
    }
}

void SmartCollectionsDialog::removeRule(RuleRowWidget* row) {
    rulesLayout_->removeWidget(row);
    auto it = std::find(rules_.begin(), rules_.end(), row);
    if (it != rules_.end())
        rules_.erase(it);
    delete row;
}

void SmartCollectionsDialog::onNewCollection() {
    clearEditor();
    nameEdit_->setFocus();
}

void SmartCollectionsDialog::onEditCollection() {
    const int64_t id = currentCollectionId();
    if (id <= 0)
        return;
    for (const lrc::SmartCollection& sc : catalog_->listSmartCollections()) {
        if (sc.id == id) {
            loadCollection(sc);
            return;
        }
    }
}

void SmartCollectionsDialog::onDeleteCollection() {
    const int64_t id = currentCollectionId();
    if (id <= 0)
        return;
    const QString name = collectionName(id);
    if (QMessageBox::question(
            this, tr("删除智能收藏夹"),
            tr("删除智能收藏夹「%1」？").arg(name)) != QMessageBox::Yes)
        return;
    if (!catalog_->deleteSmartCollection(id)) {
        QMessageBox::warning(this, tr("删除智能收藏夹"), tr("删除失败。"));
        return;
    }
    emit smartCollectionsChanged();
    refreshList();
}

void SmartCollectionsDialog::onSave() {
    const QString name = nameEdit_->text().trimmed();
    if (name.isEmpty()) {
        QMessageBox::information(this, tr("保存智能收藏夹"),
                                 tr("请输入名称。"));
        return;
    }
    if (rules_.empty() || !hasValidRules()) {
        QMessageBox::information(this, tr("保存智能收藏夹"),
                                 tr("请至少添加一条有效规则。"));
        return;
    }
    const QString predicate = buildPredicateJson();
    const QString matchType = matchCombo_->currentData().toString();
    if (editingId_ > 0) {
        if (!catalog_->updateSmartCollection(
                editingId_, name.toStdString(), predicate.toStdString(),
                matchType.toStdString())) {
            QMessageBox::warning(this, tr("保存智能收藏夹"), tr("保存失败。"));
            return;
        }
    } else {
        const int64_t id = catalog_->createSmartCollection(
            name.toStdString(), predicate.toStdString(),
            matchType.toStdString());
        if (id <= 0) {
            QMessageBox::warning(this, tr("保存智能收藏夹"), tr("创建失败。"));
            return;
        }
        editingId_ = id;
    }
    emit smartCollectionsChanged();
    refreshList();
    for (int i = 0; i < list_->count(); ++i) {
        if (list_->item(i)->data(Qt::UserRole).toLongLong() == editingId_) {
            list_->setCurrentRow(i);
            break;
        }
    }
}

void SmartCollectionsDialog::onAddRule() {
    auto* row = new RuleRowWidget(rulesContainer_);
    connect(row->removeButton(), &QPushButton::clicked, this,
            [this, row]() { removeRule(row); });
    rulesLayout_->addWidget(row);
    rules_.push_back(row);
}

void SmartCollectionsDialog::onRemoveRule() {
    if (!rules_.empty()) {
        RuleRowWidget* row = rules_.back();
        removeRule(row);
    }
}

QString SmartCollectionsDialog::buildPredicateJson() const {
    json::Value root = json::Value::object();
    root["match"] = json::Value(
        matchCombo_->currentData().toString() == QStringLiteral("ANY") ? "any"
                                                                        : "all");
    json::Value rules = json::Value::array();
    for (const RuleRowWidget* row : rules_) {
        json::Value rule = json::Value::object();
        rule["field"] = json::Value(row->field().toStdString());
        rule["op"] = json::Value(row->op().toStdString());
        rule["value"] = row->value();
        rules.push_back(std::move(rule));
    }
    root["rules"] = std::move(rules);
    return QString::fromStdString(root.serialize());
}

bool SmartCollectionsDialog::hasValidRules() const {
    for (const RuleRowWidget* row : rules_) {
        const json::Value v = row->value();
        if (v.isString() && v.asString().empty())
            return false;
        if (v.isArray() && v.size() == 0)
            return false;
    }
    return !rules_.empty();
}
