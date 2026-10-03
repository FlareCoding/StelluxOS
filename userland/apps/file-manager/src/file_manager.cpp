/* file-manager: a directory browser whose entries open on double click or
 * Enter, programs launching through the process API. */
#include <stlx/proc.h>

#include <QAbstractTableModel>
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFileIconProvider>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QTreeView>
#include <QVBoxLayout>

constexpr int WINDOW_W = 560;
constexpr int WINDOW_H = 420;
constexpr int WINDOW_MIN_W = 420;
constexpr int WINDOW_MIN_H = 300;
constexpr int TOOLBAR_MARGIN = 10;
constexpr int TOOLBAR_SPACING = 8;
constexpr int LIST_MARGIN_X = 10;
constexpr int LIST_MARGIN_Y = 4;
constexpr int STATUS_MARGIN_X = 12;
constexpr int STATUS_MARGIN_Y = 6;
constexpr int SIZE_DECIMALS = 1;

constexpr QDir::Filters ENTRY_FILTER = QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System;
constexpr QDir::SortFlags ENTRY_ORDER = QDir::DirsFirst | QDir::Name | QDir::IgnoreCase;
constexpr QFile::Permissions EXECUTE_PERMISSIONS = QFile::ExeOwner | QFile::ExeGroup | QFile::ExeOther;

namespace {

/* The entries of one directory, listed whole on each load so a change shows at once */
class directory_model : public QAbstractTableModel {
public:
    enum column { NAME_COLUMN, SIZE_COLUMN, COLUMN_COUNT };

    using QAbstractTableModel::QAbstractTableModel;

    bool load(const QString& path) {
        /* An empty path is refused, since QDir would read it as the working directory */
        QDir dir(path);
        if (path.isEmpty() || !dir.exists() || !dir.isReadable()) {
            return false;
        }

        beginResetModel();
        m_path = dir.absolutePath();
        m_entries = dir.entryInfoList(ENTRY_FILTER, ENTRY_ORDER);
        endResetModel();
        return true;
    }

    /* Deletes an entry from the disk and the listing. A directory goes only once empty */
    bool remove(const QModelIndex& index) {
        const QFileInfo& entry = m_entries.at(index.row());
        bool is_removed = entry.isDir() && !entry.isSymLink() ? QDir().rmdir(entry.filePath())
                                                              : QFile::remove(entry.filePath());
        if (!is_removed) {
            return false;
        }

        beginRemoveRows(QModelIndex(), index.row(), index.row());
        m_entries.removeAt(index.row());
        endRemoveRows();
        return true;
    }

    const QString& path() const {
        return m_path;
    }

    const QFileInfo& entry(const QModelIndex& index) const {
        return m_entries.at(index.row());
    }

    int rowCount(const QModelIndex& parent) const override {
        return parent.isValid() ? 0 : static_cast<int>(m_entries.size());
    }

    int columnCount(const QModelIndex& parent) const override {
        return parent.isValid() ? 0 : COLUMN_COUNT;
    }

    QVariant data(const QModelIndex& index, int role) const override {
        const QFileInfo& entry = m_entries.at(index.row());
        if (index.column() == NAME_COLUMN) {
            if (role == Qt::DisplayRole) {
                return entry.fileName();
            }

            return role == Qt::DecorationRole ? QVariant(m_icons.icon(entry)) : QVariant();
        }

        if (role == Qt::TextAlignmentRole) {
            return QVariant::fromValue(Qt::Alignment(Qt::AlignRight | Qt::AlignVCenter));
        }

        if (role == Qt::DisplayRole && !entry.isDir()) {
            return QLocale().formattedDataSize(entry.size(), SIZE_DECIMALS, QLocale::DataSizeTraditionalFormat);
        }

        return QVariant();
    }

private:
    QString m_path;
    QFileInfoList m_entries;
    QFileIconProvider m_icons;
};

} // namespace

struct app_state {
    directory_model* model = nullptr;
    QTreeView* view = nullptr;
    QLineEdit* path_field = nullptr;
    QLabel* status = nullptr;
    QAction* open_action = nullptr;
    QAction* remove_action = nullptr;
};

static app_state g_st;

static void set_status(const QString& text) {
    g_st.status->setText(text);
}

static void update_actions() {
    bool has_entry = g_st.view->currentIndex().isValid();
    g_st.open_action->setEnabled(has_entry);
    g_st.remove_action->setEnabled(has_entry);
}

static void navigate(const QString& path) {
    if (!g_st.model->load(path)) {
        set_status(QStringLiteral("cannot open %1").arg(path));
        return;
    }

    g_st.path_field->setText(g_st.model->path());
    update_actions();

    int count = g_st.model->rowCount(QModelIndex());
    set_status(count == 1 ? QStringLiteral("1 item") : QStringLiteral("%1 items").arg(count));
}

static void navigate_up() {
    QDir dir(g_st.model->path());
    if (dir.cdUp()) {
        navigate(dir.absolutePath());
    }
}

static void launch_program(const QFileInfo& entry) {
    int handle = proc_exec(QFile::encodeName(entry.filePath()).constData(), nullptr);
    if (handle < 0) {
        set_status(QStringLiteral("failed to launch %1").arg(entry.fileName()));
        return;
    }

    proc_detach(handle);
    set_status(QStringLiteral("launched %1").arg(entry.fileName()));
}

static void open_entry(const QModelIndex& index) {
    const QFileInfo entry = g_st.model->entry(index);
    if (entry.isDir()) {
        navigate(entry.absoluteFilePath());
        return;
    }

    // A program is a file carrying an execute permission bit
    if (entry.isFile() && entry.permissions().testAnyFlags(EXECUTE_PERMISSIONS)) {
        launch_program(entry);
        return;
    }

    set_status(QStringLiteral("no handler for %1").arg(entry.fileName()));
}

static void remove_entry(const QModelIndex& index) {
    const QFileInfo entry = g_st.model->entry(index);
    if (!g_st.model->remove(index)) {
        set_status(entry.isDir() ? QStringLiteral("cannot remove %1 (not empty or protected)").arg(entry.fileName())
                                 : QStringLiteral("cannot remove %1").arg(entry.fileName()));
        return;
    }

    update_actions();
    set_status(QStringLiteral("removed %1").arg(entry.fileName()));
}

static QWidget* make_toolbar() {
    auto* toolbar = new QWidget;
    auto* layout = new QHBoxLayout(toolbar);
    layout->setContentsMargins(TOOLBAR_MARGIN, TOOLBAR_MARGIN, TOOLBAR_MARGIN, TOOLBAR_MARGIN);
    layout->setSpacing(TOOLBAR_SPACING);

    auto* up = new QPushButton(QStringLiteral("Up"));
    QObject::connect(up, &QPushButton::clicked, navigate_up);
    layout->addWidget(up);

    g_st.path_field = new QLineEdit;
    QObject::connect(g_st.path_field, &QLineEdit::returnPressed, [] { navigate(g_st.path_field->text()); });
    layout->addWidget(g_st.path_field, 1);

    return toolbar;
}

/* The entries as rows of name and size, their actions doubling as the context menu */
static QTreeView* make_listing() {
    g_st.view = new QTreeView;
    g_st.model = new directory_model(g_st.view);

    QTreeView* view = g_st.view;
    view->setModel(g_st.model);
    view->setRootIsDecorated(false);
    view->setItemsExpandable(false);
    view->setUniformRowHeights(true);
    view->setSelectionMode(QAbstractItemView::SingleSelection);
    view->setFrameShape(QFrame::NoFrame);
    view->setContentsMargins(LIST_MARGIN_X, LIST_MARGIN_Y, LIST_MARGIN_X, LIST_MARGIN_Y);

    QHeaderView* header = view->header();
    header->hide();
    header->setStretchLastSection(false);
    header->setSectionResizeMode(directory_model::NAME_COLUMN, QHeaderView::Stretch);
    header->setSectionResizeMode(directory_model::SIZE_COLUMN, QHeaderView::ResizeToContents);

    g_st.open_action = new QAction(QStringLiteral("Open"), view);
    QObject::connect(g_st.open_action, &QAction::triggered, [] { open_entry(g_st.view->currentIndex()); });

    auto* separator = new QAction(view);
    separator->setSeparator(true);

    /* Holding the key deletes one entry, not every row that becomes current after it */
    g_st.remove_action = new QAction(QStringLiteral("Delete permanently"), view);
    g_st.remove_action->setShortcut(QKeySequence::Delete);
    g_st.remove_action->setShortcutContext(Qt::WidgetShortcut);
    g_st.remove_action->setAutoRepeat(false);
    QObject::connect(g_st.remove_action, &QAction::triggered, [] { remove_entry(g_st.view->currentIndex()); });

    view->addActions({ g_st.open_action, separator, g_st.remove_action });
    view->setContextMenuPolicy(Qt::ActionsContextMenu);

    QObject::connect(view, &QAbstractItemView::activated, open_entry);
    QObject::connect(view->selectionModel(), &QItemSelectionModel::currentChanged, [](const QModelIndex& current) {
        update_actions();
        if (current.isValid()) {
            set_status(g_st.model->entry(current).fileName());
        }
    });

    return view;
}

static QWidget* make_footer() {
    auto* footer = new QWidget;
    footer->setBackgroundRole(QPalette::Dark);
    footer->setAutoFillBackground(true);

    auto* layout = new QHBoxLayout(footer);
    layout->setContentsMargins(STATUS_MARGIN_X, STATUS_MARGIN_Y, STATUS_MARGIN_X, STATUS_MARGIN_Y);

    g_st.status = new QLabel;
    g_st.status->setFont(QFontDatabase::systemFont(QFontDatabase::SmallestReadableFont));
    g_st.status->setForegroundRole(QPalette::PlaceholderText);
    layout->addWidget(g_st.status);

    return footer;
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    QWidget window;
    window.setWindowTitle(QStringLiteral("File Manager"));
    window.resize(WINDOW_W, WINDOW_H);
    window.setMinimumSize(WINDOW_MIN_W, WINDOW_MIN_H);

    auto* layout = new QVBoxLayout(&window);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(make_toolbar());
    layout->addWidget(make_listing(), 1);
    layout->addWidget(make_footer());

    navigate(QDir::rootPath());
    window.show();

    return app.exec();
}
