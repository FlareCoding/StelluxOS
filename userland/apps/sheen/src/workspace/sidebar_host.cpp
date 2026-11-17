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

static std::pair<QWidget*, QLabel*> make_section_header(const QString& title, QWidget* parent, bool draws_top_line) {
    auto* container = new QWidget(parent);
    container->setFixedHeight(HEADER_HEIGHT);

    auto* layout = new QHBoxLayout(container);
    layout->setContentsMargins(HEADER_PADDING_X, draws_top_line ? 1 : 0, HEADER_PADDING_X, 0);
    layout->setSpacing(0);

    auto* label = new QLabel(title.toUpper(), container);
    QFont font = ui_font(STLXTHEME_CAPTION_SIZE, QFont::DemiBold);
    font.setLetterSpacing(QFont::PercentageSpacing, CAPTION_SPACING);
    label->setFont(font);

    QString top_border = draws_top_line
                       ? QStringLiteral("1px solid %1").arg(token(STLXTHEME_LINE).name(QColor::HexArgb))
                       : QStringLiteral("none");
    label->setStyleSheet(QStringLiteral("color: %1; background: transparent; border-top: %2;")
                             .arg(token(STLXTHEME_TEXT_DIM).name(), top_border));
    layout->addWidget(label, 1);

    return { container, label };
}

sidebar_host::sidebar_host(const QString& root_path, QWidget* parent)
    : QWidget(parent), m_root_path(root_path) {
    setFixedWidth(DEFAULT_SIDEBAR_WIDTH);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 1, 0);
    layout->setSpacing(0);

    layout->addWidget(make_section_header(QStringLiteral("Files"), this, false).first);

    m_file_tree = new QTreeWidget(this);
    m_file_tree->setHeaderHidden(true);
    m_file_tree->setRootIsDecorated(false);
    m_file_tree->setIndentation(0);
    m_file_tree->setFrameShape(QFrame::NoFrame);
    m_file_tree->setUniformRowHeights(true);

    m_file_tree->viewport()->setMouseTracking(true);
    m_file_tree->setItemDelegate(new file_tree_delegate(m_file_tree));
    m_file_tree->setStyleSheet(QStringLiteral("QTreeWidget { background: transparent; border: none; outline: none; }"));
    layout->addWidget(m_file_tree, 2);

    m_sections_container = new QWidget(this);
    m_sections_layout = new QVBoxLayout(m_sections_container);
    m_sections_layout->setContentsMargins(0, 0, 0, 0);
    m_sections_layout->setSpacing(0);
    m_sections_container->hide();
    layout->addWidget(m_sections_container, 1);

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

void sidebar_host::set_extension_sections(const QString& extension_id, const QList<sidebar_section_spec>& sections) {
    QList<QWidget*> incoming_widgets;
    for (const sidebar_section_spec& spec : sections) {
        if (spec.widget) {
            incoming_widgets.append(spec.widget);
        }
    }

    for (qsizetype i = m_sections.size() - 1; i >= 0; i--) {
        if (m_sections.at(i).extension_id != extension_id) {
            continue;
        }

        owned_sidebar_section& existing = m_sections[i];
        if (!incoming_widgets.contains(existing.spec.widget)) {
            m_sections_layout->removeWidget(existing.header_widget);
            existing.header_widget->deleteLater();
            if (existing.spec.widget) {
                m_sections_layout->removeWidget(existing.spec.widget);
                existing.spec.widget->hide();
                existing.spec.widget->setParent(nullptr);
                existing.spec.widget->deleteLater();
            }

            m_sections.removeAt(i);
        }
    }

    QList<owned_sidebar_section> retained_other;
    QList<owned_sidebar_section> existing_for_owner;
    for (const owned_sidebar_section& entry : m_sections) {
        if (entry.extension_id == extension_id) {
            existing_for_owner.append(entry);
        } else {
            retained_other.append(entry);
        }
    }

    QList<owned_sidebar_section> updated_owner;
    for (const sidebar_section_spec& spec : sections) {
        if (!spec.widget) {
            continue;
        }

        int match_idx = -1;
        for (int i = 0; i < existing_for_owner.size(); i++) {
            if (existing_for_owner.at(i).spec.widget == spec.widget) {
                match_idx = i;
                break;
            }
        }

        if (match_idx >= 0) {
            owned_sidebar_section reused = existing_for_owner.takeAt(match_idx);
            reused.spec = spec;
            reused.header_label->setText(spec.title.toUpper());
            updated_owner.append(reused);
        } else {
            auto [header, label] = make_section_header(spec.title, m_sections_container, true);
            spec.widget->setParent(m_sections_container);
            spec.widget->show();
            updated_owner.append({ extension_id, spec, header, label });
        }
    }

    for (const owned_sidebar_section& leftover : existing_for_owner) {
        m_sections_layout->removeWidget(leftover.header_widget);
        leftover.header_widget->deleteLater();
    }

    m_sections = retained_other;
    m_sections.append(updated_owner);
    rebuild_sections_layout();
}

void sidebar_host::remove_extension_sections(const QString& extension_id) {
    set_extension_sections(extension_id, {});
}

void sidebar_host::rebuild_sections_layout() {
    while (m_sections_layout->count() > 0) {
        m_sections_layout->takeAt(0);
    }

    for (const owned_sidebar_section& entry : m_sections) {
        m_sections_layout->addWidget(entry.header_widget);
        m_sections_layout->addWidget(entry.spec.widget, 1);
    }

    m_sections_container->setVisible(!m_sections.isEmpty());
}

void sidebar_host::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), token(STLXTHEME_SHADE));
    painter.fillRect(QRect(width() - 1, 0, 1, height()), token(STLXTHEME_LINE));
}

} // namespace sheen
