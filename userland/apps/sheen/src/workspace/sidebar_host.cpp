#include "workspace/sidebar_host.h"
#include "app/workspace_store.h"
#include "ui/chrome.h"

#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace sheen {

static constexpr int HEADER_HEIGHT = 28;
static constexpr int HEADER_PADDING_X = 12;
static constexpr int ROW_HEIGHT = STLXTHEME_LIST_ROW_HEIGHT;
static constexpr int DEPTH_INDENT = 14;
static constexpr int MAX_TREE_DEPTH = 4;
static constexpr int MAX_DIR_ENTRIES = 250;
static constexpr qreal CAPTION_SPACING = 106.0;
static constexpr int FILE_PATH_ROLE = Qt::UserRole;
static constexpr int IS_DIR_ROLE = Qt::UserRole + 1;
static constexpr int LOADED_CHILDREN_ROLE = Qt::UserRole + 2;

namespace {

class file_tree_delegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override {
        return QSize(0, ROW_HEIGHT);
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);

        int depth = 0;
        for (QModelIndex anc = index.parent(); anc.isValid(); anc = anc.parent()) {
            depth++;
        }

        const auto* tree = qobject_cast<const QTreeWidget*>(option.widget);
        int full_width = tree ? tree->viewport()->width() : option.rect.width();
        QRect full_row(4, option.rect.top() + 1, qMax(0, full_width - 8), option.rect.height() - 2);

        bool is_selected = option.state & QStyle::State_Selected;
        bool is_hovered = option.state & QStyle::State_MouseOver;
        if (is_selected || is_hovered) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(token(is_selected ? STLXTHEME_SURFACE_SELECTED : STLXTHEME_ROW_HOVER));
            painter->drawRoundedRect(full_row, STLXTHEME_MENU_ITEM_RADIUS, STLXTHEME_MENU_ITEM_RADIUS);
        }

        int text_left = HEADER_PADDING_X + depth * DEPTH_INDENT;
        int text_width = qMax(0, full_width - text_left - HEADER_PADDING_X);
        QRect label_rect(text_left, option.rect.top(), text_width, option.rect.height());

        bool is_dir = index.data(IS_DIR_ROLE).toBool();
        painter->setFont(ui_font(STLXTHEME_BODY_SIZE, is_dir ? QFont::Medium : QFont::Normal));
        painter->setPen(token(is_selected || is_hovered ? STLXTHEME_TEXT : STLXTHEME_TEXT_SECONDARY));

        QString label = index.data(Qt::DisplayRole).toString();
        if (is_dir) {
            label += QLatin1Char('/');
        }

        painter->drawText(label_rect, Qt::AlignLeft | Qt::AlignVCenter,
                          painter->fontMetrics().elidedText(label, Qt::ElideMiddle, label_rect.width()));
        painter->restore();
    }
};

} // namespace

static QWidget* make_files_header(QWidget* parent) {
    auto* container = new QWidget(parent);
    container->setFixedHeight(HEADER_HEIGHT);

    auto* layout = new QHBoxLayout(container);
    layout->setContentsMargins(HEADER_PADDING_X, 0, HEADER_PADDING_X, 0);
    layout->setSpacing(0);

    auto* label = new QLabel(QStringLiteral("FILES"), container);
    QFont font = ui_font(STLXTHEME_CAPTION_SIZE, QFont::DemiBold);
    font.setLetterSpacing(QFont::PercentageSpacing, CAPTION_SPACING);
    label->setFont(font);
    label->setStyleSheet(QStringLiteral("color: %1; background: transparent;")
                             .arg(token(STLXTHEME_TEXT_DIM).name()));
    layout->addWidget(label, 1);

    return container;
}

sidebar_host::sidebar_host(const QString& root_path, QWidget* parent)
    : QWidget(parent), m_root_path(root_path) {
    setFixedWidth(DEFAULT_SIDEBAR_WIDTH);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 1, 0);
    layout->setSpacing(0);

    layout->addWidget(make_files_header(this));

    m_file_tree = new QTreeWidget(this);
    m_file_tree->setHeaderHidden(true);
    m_file_tree->setRootIsDecorated(false);
    m_file_tree->setIndentation(0);
    m_file_tree->setFrameShape(QFrame::NoFrame);
    m_file_tree->setUniformRowHeights(true);

    m_file_tree->viewport()->setMouseTracking(true);
    m_file_tree->setItemDelegate(new file_tree_delegate(m_file_tree));
    m_file_tree->setStyleSheet(QStringLiteral("QTreeWidget { background: transparent; border: none; outline: none; }"));
    layout->addWidget(m_file_tree, 1);

    connect(m_file_tree, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem* item) {
        if (!item || item->data(0, LOADED_CHILDREN_ROLE).toBool()) {
            return;
        }

        item->setData(0, LOADED_CHILDREN_ROLE, true);
        while (item->childCount() > 0) {
            delete item->takeChild(0);
        }

        populate_directory(item->data(0, FILE_PATH_ROLE).toString(), item, 1);
    });

    auto activate_item = [this](QTreeWidgetItem* item) {
        if (!item) {
            return;
        }

        if (item->data(0, IS_DIR_ROLE).toBool()) {
            item->setExpanded(!item->isExpanded());
            return;
        }

        if (on_file_activated) {
            on_file_activated(item->data(0, FILE_PATH_ROLE).toString());
        }
    };

    connect(m_file_tree, &QTreeWidget::itemClicked, this, [activate_item](QTreeWidgetItem* item, int) {
        activate_item(item);
    });

    connect(m_file_tree, &QTreeWidget::itemActivated, this, [activate_item](QTreeWidgetItem* item, int) {
        activate_item(item);
    });

    refresh_files();
}

void sidebar_host::set_sidebar_width(int width) {
    setFixedWidth(std::clamp(width, MIN_SIDEBAR_WIDTH, MAX_SIDEBAR_WIDTH));
}

void sidebar_host::set_root_path(const QString& root_path) {
    m_root_path = root_path;
    refresh_files();
}

void sidebar_host::refresh_files() {
    QString current_path;
    if (QTreeWidgetItem* current = m_file_tree->currentItem()) {
        current_path = current->data(0, FILE_PATH_ROLE).toString();
    }

    m_file_tree->clear();
    if (!m_root_path.isEmpty()) {
        populate_directory(m_root_path, nullptr, 0);
    }

    if (!current_path.isEmpty()) {
        highlight_active_file(current_path);
    }
}

void sidebar_host::populate_directory(const QString& dir_path, QTreeWidgetItem* parent_item, int depth) {
    QDir dir(dir_path);
    QFileInfoList entries = dir.entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot,
                                              QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);
    int added = 0;
    for (const QFileInfo& info : entries) {
        if (added >= MAX_DIR_ENTRIES) {
            break;
        }

        added++;
        auto* item = parent_item ? new QTreeWidgetItem(parent_item) : new QTreeWidgetItem(m_file_tree);
        item->setText(0, info.fileName());
        item->setToolTip(0, info.absoluteFilePath());
        item->setData(0, FILE_PATH_ROLE, info.absoluteFilePath());
        item->setData(0, IS_DIR_ROLE, info.isDir());

        if (info.isDir()) {
            if (depth + 1 < MAX_TREE_DEPTH) {
                item->setData(0, LOADED_CHILDREN_ROLE, true);
                populate_directory(info.absoluteFilePath(), item, depth + 1);
            } else {
                item->setData(0, LOADED_CHILDREN_ROLE, false);
                new QTreeWidgetItem(item);
            }
        }
    }
}

void sidebar_host::highlight_active_file(const QString& file_path) {
    if (file_path.isEmpty()) {
        m_file_tree->clearSelection();
        return;
    }

    QString target = QFileInfo(file_path).absoluteFilePath();
    QList<QTreeWidgetItem*> stack;
    for (int i = 0; i < m_file_tree->topLevelItemCount(); i++) {
        stack.append(m_file_tree->topLevelItem(i));
    }

    while (!stack.isEmpty()) {
        QTreeWidgetItem* item = stack.takeLast();
        if (item->data(0, FILE_PATH_ROLE).toString() == target) {
            m_file_tree->setCurrentItem(item);
            return;
        }

        for (int c = 0; c < item->childCount(); c++) {
            stack.append(item->child(c));
        }
    }
}

void sidebar_host::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), token(STLXTHEME_SHADE));
    painter.fillRect(QRect(width() - 1, 0, 1, height()), token(STLXTHEME_LINE));
}

} // namespace sheen
