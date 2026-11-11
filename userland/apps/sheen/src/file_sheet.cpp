#include "file_sheet.h"
#include "chrome.h"
#include "keycaps.h"

#include <QAbstractTableModel>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileIconProvider>
#include <QHash>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMimeDatabase>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>

#include <Scintilla.h>

#include <sys/stat.h>

enum class place_glyph { CLOCK, HOUSE, DISK, BOX, HOURGLASS };

struct sheet_place {
    const char* label;
    const char* path; // empty for this session's recent files
    place_glyph glyph;
};

static constexpr int SHEET_WIDTH = 900;
static constexpr int SHEET_HEIGHT = 620;
static constexpr int PLACES_WIDTH = 184;
static constexpr int NAME_MIN_WIDTH = 200;
static constexpr int HEADER_MARGIN_X = 20;
static constexpr int HEADER_MARGIN_TOP = 14;
static constexpr int SECTION_GAP = 8;
static constexpr int SIZE_DECIMALS = 1;
static constexpr qint64 BYTES_PER_KB = 1024;
static constexpr qreal MATCH_ALPHA = 0.13;
static constexpr qreal MATCH_RADIUS = 2;
static constexpr int PLACE_PATH_INSET = 10;
static constexpr int COMBO_MIN_WIDTH = 120;
static constexpr int COLUMN_HEADER_HEIGHT = 30;
static constexpr int COLUMN_PADDING_X = 4;
static constexpr qreal TITLE_LETTER_SPACING = 106; // percent, 0.06 em between letters
static constexpr qreal SORT_ARROW_GAP = 4;
static constexpr qreal SORT_ARROW_HALF = 3.5;
static constexpr qreal SORT_ARROW_HEAD = 2.5;
static constexpr int PLACE_ICON_SIZE = 16;
static constexpr qreal PLACE_GLYPH_STROKE = 1.25;
static constexpr int STEP_BUTTON_WIDTH = 30;
static constexpr int STEP_BOX_HEIGHT = 28;
static constexpr int STEP_BORDER = 1;
static constexpr qreal STEP_CHEVRON_HALF = 4;
static constexpr qreal STEP_CHEVRON_DEPTH = 2;
static constexpr qreal STEP_ARROW_HALF = 4.5;
static constexpr qreal GLYPH_STROKE = 1.5;
static constexpr qreal HALF_PIXEL = 0.5;

static const char* const MODIFIED_FORMAT = "yyyy-MM-dd HH:mm";
static const char* const SIZE_UNITS[] = { "KB", "MB", "GB" };

static constexpr QDir::Filters ENTRY_FILTER = QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System;
static constexpr QDir::SortFlags ENTRY_ORDER = QDir::DirsFirst | QDir::Name | QDir::IgnoreCase;

static const sheet_place PLACES[] = {
    { "Recent", "", place_glyph::CLOCK },
    { "Home", "/", place_glyph::HOUSE },
    { "Root", "/", place_glyph::DISK },
    { "Resources", "/etc/res", place_glyph::BOX },
    { "Temporary", "/tmp", place_glyph::HOURGLASS },
};

struct eol_entry {
    int mode;
    const char* label;
};

static const eol_entry EOL_ENTRIES[] = {
    { SC_EOL_LF, "LF" },
    { SC_EOL_CRLF, "CRLF" },
    { SC_EOL_CR, "CR" },
};

static const int BOM_ROLE = Qt::UserRole + 1;

/* One folder's entries, or this session's recent files, narrowed by a filter typed over the list */
class folder_model : public QAbstractTableModel {
public:
    enum column { NAME, SIZE, KIND, MODIFIED, COLUMN_COUNT };

    using QAbstractTableModel::QAbstractTableModel;

    void show_entries(const QFileInfoList& entries) {
        beginResetModel();
        m_entries = entries;
        m_counts.clear();
        m_filter.clear();
        apply_filter();
        endResetModel();
    }

    void set_filter(const QString& filter) {
        beginResetModel();
        m_filter = filter;
        apply_filter();
        endResetModel();
    }

    const QFileInfo& entry(int row) const {
        return m_shown.at(row);
    }

    int rowCount(const QModelIndex& parent) const override {
        return parent.isValid() ? 0 : static_cast<int>(m_shown.size());
    }

    int columnCount(const QModelIndex& parent) const override {
        return parent.isValid() ? 0 : COLUMN_COUNT;
    }

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
        static const char* const TITLES[] = { "Name", "Size", "Kind", "Modified" };
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
            return QVariant();
        }

        return QString::fromLatin1(TITLES[section]);
    }

    QVariant data(const QModelIndex& index, int role) const override {
        const QFileInfo& entry = m_shown.at(index.row());
        if (role == Qt::ToolTipRole) {
            return entry.absoluteFilePath();
        }

        if (index.column() == NAME && role == Qt::DecorationRole) {
            return m_icons.icon(entry);
        }

        if (index.column() == SIZE && role == Qt::TextAlignmentRole) {
            return QVariant::fromValue(Qt::Alignment(Qt::AlignRight | Qt::AlignVCenter));
        }

        if (role != Qt::DisplayRole) {
            return QVariant();
        }

        switch (index.column()) {
            case NAME:
                return entry.fileName();
            case SIZE:
                return entry.isDir() ? item_count(entry) : size_label(entry.size());
            case KIND:
                return entry.isDir() ? QStringLiteral("Folder") : kind_of(entry);
            case MODIFIED:
                return entry.lastModified().toString(QString::fromLatin1(MODIFIED_FORMAT));
            default:
                return QVariant();
        }
    }

private:
    // The database names some types only by their MIME name, which reads poorly as a kind
    QString kind_of(const QFileInfo& entry) const {
        QMimeType type = m_mimes.mimeTypeForFile(entry, QMimeDatabase::MatchExtension);
        if (type.comment() != type.name()) {
            QString kind = type.comment();
            kind[0] = kind[0].toUpper();
            return kind;
        }

        return type.inherits(QStringLiteral("text/plain")) ? QStringLiteral("Plain text") : QStringLiteral("File");
    }

    // Bytes under a kilobyte, then one decimal of the largest unit that keeps the number under 1024
    static QString size_label(qint64 bytes) {
        if (bytes < BYTES_PER_KB) {
            return QStringLiteral("%1 B").arg(bytes);
        }

        double size = static_cast<double>(bytes) / BYTES_PER_KB;
        int unit = 0;
        while (size >= BYTES_PER_KB && unit + 1 < static_cast<int>(std::size(SIZE_UNITS))) {
            size /= BYTES_PER_KB;
            unit++;
        }

        QString number = QString::number(size, 'f', SIZE_DECIMALS);
        return QStringLiteral("%1 %2").arg(number, QString::fromLatin1(SIZE_UNITS[unit]));
    }

    // A folder's size is how many entries it holds, counted once per listing
    QString item_count(const QFileInfo& folder) const {
        QString path = folder.absoluteFilePath();
        if (!m_counts.contains(path)) {
            qsizetype count = QDir(path).entryList(ENTRY_FILTER).size();
            m_counts.insert(path, count == 1 ? QStringLiteral("1 item") : QStringLiteral("%1 items").arg(count));
        }

        return m_counts.value(path);
    }

    void apply_filter() {
        m_shown.clear();
        for (const QFileInfo& entry : m_entries) {
            if (entry.fileName().contains(m_filter, Qt::CaseInsensitive)) {
                m_shown.append(entry);
            }
        }
    }

    QFileInfoList m_entries;
    QFileInfoList m_shown;
    QString m_filter;
    QFileIconProvider m_icons;
    QMimeDatabase m_mimes;
    mutable QHash<QString, QString> m_counts;
};

/* Marks where each name holds the filter typed over the list */
class name_match_delegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void set_filter(const QString& filter) {
        m_filter = filter;
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        QStyledItemDelegate::paint(painter, option, index);
        if (m_filter.isEmpty()) {
            return;
        }

        QStyleOptionViewItem item = option;
        initStyleOption(&item, index);
        qsizetype at = item.text.indexOf(m_filter, 0, Qt::CaseInsensitive);
        if (at < 0) {
            return;
        }

        // The style insets an item's text by its focus frame margin and one pixel more
        QStyle* style = item.widget->style();
        QRect text = style->subElementRect(QStyle::SE_ItemViewItemText, &item, item.widget);
        QFontMetricsF metrics(item.font);
        qreal left = text.left() + style->pixelMetric(QStyle::PM_FocusFrameHMargin, nullptr, item.widget) + 1
                   + metrics.horizontalAdvance(item.text.left(at));
        qreal width = metrics.horizontalAdvance(item.text.mid(at, m_filter.size()));
        QRectF mark(left, text.center().y() - metrics.height() / 2, width, metrics.height());

        QColor tint = token(STLXTHEME_TEXT);
        tint.setAlphaF(MATCH_ALPHA);

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(Qt::NoPen);
        painter->setBrush(tint);
        painter->drawRoundedRect(mark, MATCH_RADIUS, MATCH_RADIUS);
        painter->restore();
    }

private:
    QString m_filter;
};

/* Each place's folder trails its name, dim, so the place says where it leads */
class place_delegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        QStyledItemDelegate::paint(painter, option, index);
        QString path = index.data(Qt::UserRole).toString();
        if (path.isEmpty()) {
            return;
        }

        QString shown = index.data().toString() == QLatin1String("Home") ? QStringLiteral("~") : path;
        QRect room = option.rect.adjusted(0, 0, -PLACE_PATH_INSET, 0);

        painter->save();
        painter->setFont(mono_font(STLXTHEME_CAPTION_SIZE));
        painter->setPen(token(STLXTHEME_TEXT_DIM));
        painter->drawText(room, Qt::AlignRight | Qt::AlignVCenter, shown);
        painter->restore();
    }
};

// An upward arrow with a stem, `half` its height above and below `center` and `head` the reach of each arm
static void paint_up_arrow(QPainter* painter, QPointF center, qreal half, qreal head) {
    QPointF tip = center - QPointF(0, half);
    QPointF arms[] = { tip + QPointF(-head, head), tip, tip + QPointF(head, head) };
    painter->drawLine(center + QPointF(0, half), tip);
    painter->drawPolyline(arms, 3);
}

/* Column titles in dim small capitals over a hairline, Name lit with the arrow of the order names follow */
class column_header : public QHeaderView {
public:
    explicit column_header(QWidget* parent) : QHeaderView(Qt::Horizontal, parent) {
        QFont titles = ui_font(STLXTHEME_CAPTION_SIZE, QFont::DemiBold);
        titles.setCapitalization(QFont::AllUppercase);
        titles.setLetterSpacing(QFont::PercentageSpacing, TITLE_LETTER_SPACING);
        setFont(titles);
        viewport()->setAutoFillBackground(false);
    }

    QSize sizeHint() const override {
        return QSize(QHeaderView::sizeHint().width(), COLUMN_HEADER_HEIGHT);
    }

protected:
    void paintEvent(QPaintEvent* event) override {
        QHeaderView::paintEvent(event);
        QPainter painter(viewport());
        painter.fillRect(QRect(0, viewport()->height() - 1, viewport()->width(), 1), token(STLXTHEME_LINE_SUBTLE));
    }

    void paintSection(QPainter* painter, const QRect& rect, int logical_index) const override {
        bool is_sorted = logical_index == folder_model::NAME;
        QString title = model()->headerData(logical_index, Qt::Horizontal).toString();
        QRect room = rect.adjusted(COLUMN_PADDING_X, 0, -COLUMN_PADDING_X, 0);
        Qt::Alignment side = logical_index == folder_model::SIZE ? Qt::AlignRight : Qt::AlignLeft;

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setFont(font());
        painter->setPen(token(is_sorted ? STLXTHEME_TEXT : STLXTHEME_TEXT_DIM));
        painter->drawText(room, side | Qt::AlignVCenter, title);
        if (is_sorted) {
            qreal x = room.left() + QFontMetricsF(font()).horizontalAdvance(title) + SORT_ARROW_GAP + SORT_ARROW_HEAD;
            painter->setPen(QPen(token(STLXTHEME_TEXT), GLYPH_STROKE, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            paint_up_arrow(painter, QPointF(x, QRectF(rect).center().y()), SORT_ARROW_HALF, SORT_ARROW_HEAD);
        }

        painter->restore();
    }
};

/* One glyph of the boxed back, forward and up control, chevrons through the history and an arrow up a folder */
class step_button : public QToolButton {
public:
    step_button(Qt::ArrowType arrow, const QString& tip, QWidget* parent) : QToolButton(parent), m_arrow(arrow) {
        setFixedSize(STEP_BUTTON_WIDTH, STEP_BOX_HEIGHT - 2 * STEP_BORDER);
        setToolTip(tip);
        setFocusPolicy(Qt::NoFocus);
    }

protected:
    void enterEvent(QEnterEvent* event) override {
        QToolButton::enterEvent(event);
        update();
    }

    void leaveEvent(QEvent* event) override {
        QToolButton::leaveEvent(event);
        update();
    }

    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        uint32_t tone = !isEnabled() ? STLXTHEME_TEXT_DISABLED : underMouse() ? STLXTHEME_TEXT : STLXTHEME_TEXT_SECONDARY;
        painter.setPen(QPen(token(tone), GLYPH_STROKE, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));

        QPointF center = QRectF(rect()).center();
        if (m_arrow == Qt::UpArrow) {
            paint_up_arrow(&painter, center, STEP_ARROW_HALF, STEP_CHEVRON_HALF);
            return;
        }

        qreal side = m_arrow == Qt::LeftArrow ? -1 : 1;
        QPointF chevron[] = { center + QPointF(-side * STEP_CHEVRON_DEPTH, -STEP_CHEVRON_HALF),
                              center + QPointF(side * STEP_CHEVRON_DEPTH, 0),
                              center + QPointF(-side * STEP_CHEVRON_DEPTH, STEP_CHEVRON_HALF) };
        painter.drawPolyline(chevron, 3);
    }

private:
    Qt::ArrowType m_arrow;
};

/* Back, forward and up joined in one raised box, hairlines between them */
class step_box : public QWidget {
public:
    explicit step_box(QWidget* parent) : QWidget(parent), m_layout(new QHBoxLayout(this)) {
        m_layout->setContentsMargins(STEP_BORDER, STEP_BORDER, STEP_BORDER, STEP_BORDER);
        m_layout->setSpacing(STEP_BORDER);
    }

    void add(QToolButton* button) {
        m_layout->addWidget(button);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        QRectF edge = QRectF(rect()).adjusted(HALF_PIXEL, HALF_PIXEL, -HALF_PIXEL, -HALF_PIXEL);
        painter.setPen(QPen(token(STLXTHEME_LINE), STEP_BORDER));
        painter.setBrush(token(STLXTHEME_SURFACE));
        painter.drawRoundedRect(edge, STLXTHEME_CONTROL_RADIUS - HALF_PIXEL, STLXTHEME_CONTROL_RADIUS - HALF_PIXEL);

        for (int index = 1; index < m_layout->count(); index++) {
            int x = m_layout->itemAt(index)->geometry().left() - STEP_BORDER;
            painter.fillRect(QRect(x, STEP_BORDER, STEP_BORDER, height() - 2 * STEP_BORDER), token(STLXTHEME_LINE));
        }
    }

private:
    QHBoxLayout* m_layout;
};

static QLabel* make_caption(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setFont(ui_font(STLXTHEME_CAPTION_SIZE));
    label->setStyleSheet(QStringLiteral("color: %1").arg(token(STLXTHEME_TEXT_DIM).name()));
    return label;
}

// The places' glyphs on a 16 px grid: a clock, a house, a disk, a box and an hourglass
static void paint_place_glyph(QPainter* painter, place_glyph glyph) {
    switch (glyph) {
        case place_glyph::CLOCK:
            painter->drawEllipse(QPointF(8, 8), 5.75, 5.75);
            painter->drawPolyline(QPolygonF({ QPointF(8, 4.75), QPointF(8, 8), QPointF(10.25, 9.5) }));
            break;
        case place_glyph::HOUSE:
            painter->drawPolyline(QPolygonF({ QPointF(2.5, 7.5), QPointF(8, 2.75), QPointF(13.5, 7.5) }));
            painter->drawPolyline(QPolygonF({ QPointF(4, 6.25), QPointF(4, 13.25), QPointF(12, 13.25), QPointF(12, 6.25) }));
            break;
        case place_glyph::DISK:
            painter->drawRoundedRect(QRectF(2.25, 4.75, 11.5, 6.5), 1.5, 1.5);
            painter->drawLine(QPointF(10.25, 8), QPointF(11, 8));
            break;
        case place_glyph::BOX:
            painter->drawRect(QRectF(2.75, 3, 10.5, 3));
            painter->drawPolyline(QPolygonF({ QPointF(3.75, 6), QPointF(3.75, 13), QPointF(12.25, 13), QPointF(12.25, 6) }));
            painter->drawLine(QPointF(6.5, 8.75), QPointF(9.5, 8.75));
            break;
        case place_glyph::HOURGLASS:
            painter->drawLine(QPointF(4, 2.75), QPointF(12, 2.75));
            painter->drawLine(QPointF(4, 13.25), QPointF(12, 13.25));
            painter->drawPolyline(QPolygonF({ QPointF(5, 2.75), QPointF(5, 4.5), QPointF(11, 11.5), QPointF(11, 13.25) }));
            painter->drawPolyline(QPolygonF({ QPointF(11, 2.75), QPointF(11, 4.5), QPointF(5, 11.5), QPointF(5, 13.25) }));
            break;
    }
}

// A place's glyph in the dim tone, lit while its place is selected
static QIcon place_icon(place_glyph glyph) {
    QIcon icon;
    for (QIcon::Mode mode : { QIcon::Normal, QIcon::Selected }) {
        QPixmap pixmap(PLACE_ICON_SIZE, PLACE_ICON_SIZE);
        pixmap.fill(Qt::transparent);

        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        QColor tone = token(mode == QIcon::Selected ? STLXTHEME_TEXT : STLXTHEME_TEXT_DIM);
        painter.setPen(QPen(tone, PLACE_GLYPH_STROKE, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        paint_place_glyph(&painter, glyph);
        painter.end();

        icon.addPixmap(pixmap, mode);
    }

    return icon;
}

file_sheet::file_sheet(QWidget* window, file_sheet_mode mode, const QString& folder, const QStringList& recent)
    : sheet(window, SHEET_WIDTH), m_mode(mode), m_model(new folder_model(this)), m_recent(recent) {
    bool is_open = mode == file_sheet_mode::OPEN;
    auto* layout = new QVBoxLayout(panel());
    layout->setContentsMargins(HEADER_MARGIN_X, HEADER_MARGIN_TOP, HEADER_MARGIN_X, HEADER_MARGIN_TOP);
    layout->setSpacing(SECTION_GAP);

    auto* header = new QHBoxLayout;
    m_title = new QLabel(is_open ? QStringLiteral("Open") : QStringLiteral("Save As"), panel());
    m_title->setFont(ui_font(STLXTHEME_BODY_SIZE, QFont::DemiBold));
    header->addWidget(m_title);

    m_note = make_caption(QString(), panel());
    header->addSpacing(SECTION_GAP);
    header->addWidget(m_note);
    header->addStretch(1);

    m_filter_hint = make_caption(QString(), panel());
    header->addWidget(m_filter_hint);
    header->addSpacing(SECTION_GAP);
    header->addWidget(new keycap_hint({ QStringLiteral("Ctrl"), QStringLiteral("L") }, QStringLiteral("type a path"),
                                      panel()));
    layout->addLayout(header);

    if (!is_open) {
        auto* name_row = new QHBoxLayout;
        name_row->addWidget(make_caption(QStringLiteral("Name"), panel()));
        m_name = new QLineEdit(panel());
        name_row->addWidget(m_name, 1);
        layout->addLayout(name_row);

        connect(m_name, &QLineEdit::textEdited, this, [this] {
            m_replace_path.clear();
            refresh_primary();
        });

        connect(m_name, &QLineEdit::returnPressed, this, [this] { accept(); });
    }

    auto* toolbar = new QHBoxLayout;
    auto* steps = new step_box(panel());
    m_back = new step_button(Qt::LeftArrow, QStringLiteral("Back"), steps);
    m_forward = new step_button(Qt::RightArrow, QStringLiteral("Forward"), steps);
    auto* parent_button = new step_button(Qt::UpArrow, QStringLiteral("Parent folder (Alt Up)"), steps);

    steps->add(m_back);
    steps->add(m_forward);
    steps->add(parent_button);
    toolbar->addWidget(steps);

    m_path = new QLineEdit(panel());
    m_path->setFont(mono_font(STLXTHEME_CAPTION_SIZE));
    m_path->installEventFilter(this);
    toolbar->addWidget(m_path, 1);
    layout->addLayout(toolbar);

    connect(m_back, &QToolButton::clicked, this, [this] { step_history(-1); });
    connect(m_forward, &QToolButton::clicked, this, [this] { step_history(1); });
    connect(parent_button, &QToolButton::clicked, this, [this] {
        QDir dir(current_folder());
        if (dir.cdUp()) {
            show_folder(dir.absolutePath(), true);
        }
    });
    connect(m_path, &QLineEdit::returnPressed, this, [this] { accept_path_field(); });

    auto* body = new QHBoxLayout;
    m_places = new QListWidget(panel());
    m_places->setFixedWidth(PLACES_WIDTH);
    m_places->setFrameShape(QFrame::NoFrame);
    m_places->setIconSize(QSize(PLACE_ICON_SIZE, PLACE_ICON_SIZE));
    m_places->setItemDelegate(new place_delegate(m_places));

    for (const sheet_place& entry : PLACES) {
        auto* item = new QListWidgetItem(place_icon(entry.glyph), QString::fromLatin1(entry.label), m_places);
        item->setData(Qt::UserRole, QString::fromLatin1(entry.path));
    }

    connect(m_places, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        QString path = item->data(Qt::UserRole).toString();
        path.isEmpty() ? show_recent() : show_folder(path, true);
        m_list->setFocus();
    });
    auto* side = new QVBoxLayout;
    side->addWidget(make_caption(QStringLiteral("PLACES"), panel()));
    side->addWidget(m_places, 1);
    body->addLayout(side);

    m_list = new QTreeView(panel());
    m_list->setModel(m_model);
    m_list->setRootIsDecorated(false);
    m_list->setItemsExpandable(false);
    m_list->setUniformRowHeights(true);
    m_list->setFrameShape(QFrame::NoFrame);

    m_matches = new name_match_delegate(m_list);
    m_list->setItemDelegateForColumn(folder_model::NAME, m_matches);

    m_list->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_list->setAllColumnsShowFocus(true);
    m_list->setSelectionMode(is_open ? QAbstractItemView::ExtendedSelection : QAbstractItemView::SingleSelection);
    m_list->setHeader(new column_header(m_list));
    m_list->header()->setStretchLastSection(false);
    m_list->header()->setSectionResizeMode(folder_model::NAME, QHeaderView::Stretch);

    m_list->installEventFilter(this);
    m_list->viewport()->installEventFilter(this);
    connect(m_list, &QTreeView::doubleClicked, this, [this] { activate_current(); });
    body->addWidget(m_list, 1);
    layout->addLayout(body, 1);

    auto* footer = new QHBoxLayout;
    footer->addWidget(make_caption(QStringLiteral("Encoding"), panel()));
    m_encoding = new QComboBox(panel());
    m_encoding->setMinimumWidth(COMBO_MIN_WIDTH);
    if (is_open) {
        m_encoding->addItem(QStringLiteral("Detect"), QByteArray());
    }

    for (const QByteArray& name : available_encodings()) {
        m_encoding->addItem(encoding_label(name), name);
        if (!is_open && name == "UTF-8") {
            m_encoding->addItem(encoding_label(file_encoding { name, true }), name);
            m_encoding->setItemData(m_encoding->count() - 1, true, BOM_ROLE);
        }
    }

    footer->addWidget(m_encoding);
    if (!is_open) {
        footer->addSpacing(SECTION_GAP);
        footer->addWidget(make_caption(QStringLiteral("Line endings"), panel()));
        m_eol = new QComboBox(panel());
        for (const eol_entry& entry : EOL_ENTRIES) {
            m_eol->addItem(QString::fromLatin1(entry.label), entry.mode);
        }

        footer->addWidget(m_eol);
    }

    footer->addSpacing(SECTION_GAP);
    m_footer_note = new QLabel(panel());
    m_footer_note->setFont(ui_font(STLXTHEME_BODY_SIZE));
    footer->addWidget(m_footer_note, 1);

    auto* cancel_button = new QPushButton(QStringLiteral("Cancel"), panel());
    connect(cancel_button, &QPushButton::clicked, this, [this] { finish(0); });
    footer->addWidget(cancel_button);

    // Opening changes nothing, so only Save takes the amber of a pending change
    m_primary = new QPushButton(is_open ? QStringLiteral("Open") : QStringLiteral("Save"), panel());
    m_primary->setDefault(!is_open);
    connect(m_primary, &QPushButton::clicked, this, [this] { accept(); });
    footer->addWidget(m_primary);
    layout->addLayout(footer);

    connect(m_list->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this] {
        if (m_name && !m_list->selectionModel()->selectedRows().isEmpty()) {
            const QFileInfo& entry = m_model->entry(m_list->selectionModel()->selectedRows().first().row());
            if (entry.isFile()) {
                m_name->setText(entry.fileName());
                m_replace_path.clear();
            }
        }

        refresh_primary();
    });

    set_panel_height(SHEET_HEIGHT);
    QString start = QFileInfo(folder).isDir() ? folder : QDir::rootPath();
    show_folder(QDir(start).absolutePath(), true);
}

bool file_sheet::choose_to_open(open_request* request, const QString& current_path) {
    m_current_path = current_path;
    point_at_start();
    set_focus_target(m_list);
    if (run() == 0) {
        return false;
    }

    *request = m_open;
    return true;
}

bool file_sheet::choose_to_save(const QString& name, const file_encoding& encoding, int eol_mode,
                                save_request* request) {
    m_name->setText(name);
    int dot = static_cast<int>(name.lastIndexOf(QLatin1Char('.')));
    m_name->setSelection(0, dot > 0 ? dot : static_cast<int>(name.size()));
    for (int index = 0; index < m_encoding->count(); index++) {
        bool has_bom = m_encoding->itemData(index, BOM_ROLE).toBool();
        bool is_same_bom = has_bom == (encoding.has_bom && encoding.name == "UTF-8");
        if (m_encoding->itemData(index).toByteArray() == encoding.name && is_same_bom) {
            m_encoding->setCurrentIndex(index);
        }
    }

    m_eol->setCurrentIndex(m_eol->findData(eol_mode));
    refresh_primary();
    set_focus_target(m_name);
    if (run() == 0) {
        return false;
    }

    *request = m_save;
    return true;
}

// Keys over the list narrow it, open and climb, and Tab completes the path field, so the sheet needs no mouse
bool file_sheet::eventFilter(QObject* watched, QEvent* event) {
    bool is_key = event->type() == QEvent::KeyPress;
    bool is_tab = is_key && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Tab;
    if (watched == m_path && is_tab && complete_path()) {
        return true;
    }

    if (watched == m_list->viewport() && event->type() == QEvent::Resize) {
        fit_columns();
    }

    if (watched != m_list || !is_key) {
        return sheet::eventFilter(watched, event);
    }

    auto* key = static_cast<QKeyEvent*>(event);
    QString filter = m_filter_hint->property("filter").toString();
    bool has_command_modifier = key->modifiers() & (Qt::ControlModifier | Qt::AltModifier);
    if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
        activate_current();
        return true;
    }

    if ((key->key() == Qt::Key_Up && (key->modifiers() & Qt::AltModifier))
        || (key->key() == Qt::Key_Backspace && filter.isEmpty())) {
        QDir dir(current_folder());
        if (dir.cdUp()) {
            show_folder(dir.absolutePath(), true);
        }

        return true;
    }

    if (key->key() == Qt::Key_Backspace) {
        set_filter(filter.chopped(1));
        return true;
    }

    if (key->key() == Qt::Key_L && key->modifiers() == Qt::ControlModifier) {
        m_path->setFocus();
        m_path->selectAll();
        return true;
    }

    QString text = key->text();
    if (has_command_modifier || text.size() != 1 || !text.at(0).isPrint()) {
        return sheet::eventFilter(watched, event);
    }

    if (text == QLatin1String("/") && filter.isEmpty()) {
        m_path->setFocus();
        m_path->setText(text);
        return true;
    }

    set_filter(filter + text);
    return true;
}

// Esc steps back one stage at a time: the Replace question, then the filter, then the sheet itself
void file_sheet::cancel() {
    if (!m_replace_path.isEmpty()) {
        m_replace_path.clear();
        refresh_primary();
        m_name->setFocus();
        return;
    }

    if (!m_filter_hint->property("filter").toString().isEmpty()) {
        set_filter(QString());
        return;
    }

    sheet::cancel();
}

void file_sheet::show_folder(const QString& path, bool is_new_step) {
    QDir dir(path);
    if (!dir.exists()) {
        return;
    }

    m_model->show_entries(dir.entryInfoList(ENTRY_FILTER, ENTRY_ORDER));
    m_path->setText(dir.absolutePath());
    m_path->setProperty("folder", dir.absolutePath());
    set_filter(QString());
    if (is_new_step) {
        m_history = m_history.mid(0, m_history_index + 1);
        m_history.append(dir.absolutePath());
        m_history_index = static_cast<int>(m_history.size()) - 1;
    }

    m_back->setEnabled(m_history_index > 0);
    m_forward->setEnabled(m_history_index + 1 < m_history.size());
    point_at_start();
    for (int column = folder_model::SIZE; column < folder_model::COLUMN_COUNT; column++) {
        m_list->resizeColumnToContents(column);
    }

    refresh_primary();
}

void file_sheet::show_recent() {
    QFileInfoList entries;
    for (const QString& path : m_recent) {
        if (QFileInfo::exists(path)) {
            entries.append(QFileInfo(path));
        }
    }

    m_model->show_entries(entries);
    m_path->setText(QString());
    m_path->setProperty("folder", QString());
    set_filter(QString());
    point_at_start();
    refresh_primary();
}

void file_sheet::set_filter(const QString& filter) {
    m_filter_hint->setProperty("filter", filter);
    m_filter_hint->setText(filter.isEmpty() ? QString() : QStringLiteral("Showing names with \"%1\"").arg(filter));
    m_model->set_filter(filter);
    m_matches->set_filter(filter);
    point_at_start();
}

void file_sheet::step_history(int direction) {
    int index = m_history_index + direction;
    if (index < 0 || index >= m_history.size()) {
        return;
    }

    m_history_index = index;
    show_folder(m_history.at(index), false);
}

void file_sheet::activate_current() {
    QModelIndex current = m_list->currentIndex();
    if (!current.isValid()) {
        return;
    }

    const QFileInfo entry = m_model->entry(current.row());
    if (entry.isDir()) {
        show_folder(entry.absoluteFilePath(), true);
        return;
    }

    if (m_mode == file_sheet_mode::SAVE_AS) {
        m_name->setText(entry.fileName());
    }

    accept();
}

// A folder typed into the path field opens in the list, a file is taken as the choice itself
void file_sheet::accept_path_field() {
    QString typed = m_path->text().trimmed();
    QFileInfo target(QDir(current_folder().isEmpty() ? QDir::rootPath() : current_folder()).absoluteFilePath(typed));
    if (target.isDir()) {
        show_folder(target.absoluteFilePath(), true);
        m_list->setFocus();
        return;
    }

    if (m_mode == file_sheet_mode::SAVE_AS) {
        show_folder(target.absolutePath(), true);
        m_name->setText(target.fileName());
        accept();
        return;
    }

    if (!target.isFile()) {
        QString refusal = refusal_by_kind(target.absoluteFilePath());
        m_footer_note->setText(refusal.isEmpty() ? QStringLiteral("%1 does not exist").arg(typed)
                                                 : QStringLiteral("%1: %2").arg(typed, refusal));
        return;
    }

    m_open = { { target.absoluteFilePath() }, m_encoding->currentData().toByteArray() };
    finish(1);
}

// Completes the typed path to the longest name its matches share, as a shell does, and a lone folder gains its /.
// Returns whether that added anything, so a Tab with nothing to complete moves on
bool file_sheet::complete_path() {
    QString typed = m_path->text();
    QString absolute = QDir(current_folder().isEmpty() ? QDir::rootPath() : current_folder()).absoluteFilePath(typed);
    bool is_folder_typed = typed.endsWith(QLatin1Char('/'));
    QString folder = is_folder_typed ? absolute : QFileInfo(absolute).absolutePath();
    QString stem = is_folder_typed ? QString() : QFileInfo(absolute).fileName();

    QStringList matches;
    for (const QString& name : QDir(folder).entryList(ENTRY_FILTER, ENTRY_ORDER)) {
        if (name.startsWith(stem)) {
            matches.append(name);
        }
    }

    if (matches.isEmpty()) {
        return false;
    }

    QString common = matches.first();
    for (const QString& match : matches) {
        while (!match.startsWith(common)) {
            common.chop(1);
        }
    }

    QString completed = QDir(folder).filePath(common);
    bool adds_slash = matches.size() == 1 && QFileInfo(completed).isDir();
    if (common.size() <= stem.size() && !adds_slash) {
        return false;
    }

    m_path->setText(adds_slash ? completed + QLatin1Char('/') : completed);
    return true;
}

// Names keep room to read, so as the list narrows Kind gives its column up first and Modified next
void file_sheet::fit_columns() {
    int room = m_list->viewport()->width() - NAME_MIN_WIDTH;
    int taken = 0;
    bool fits = true;
    for (int column : { folder_model::SIZE, folder_model::MODIFIED, folder_model::KIND }) {
        m_list->setColumnHidden(column, false);
        m_list->resizeColumnToContents(column);
        taken += m_list->columnWidth(column);
        fits = fits && (column == folder_model::SIZE || taken <= room);
        m_list->setColumnHidden(column, !fits);
    }
}

void file_sheet::accept() {
    if (m_mode == file_sheet_mode::OPEN) {
        QStringList files = selected_files();
        if (files.isEmpty()) {
            return;
        }

        m_open = { files, m_encoding->currentData().toByteArray() };
        finish(1);
        return;
    }

    QString name = m_name->text().trimmed();
    if (name.isEmpty()) {
        return;
    }

    QFileInfo target(QDir(current_folder().isEmpty() ? QDir::rootPath() : current_folder()).absoluteFilePath(name));
    if (target.isDir()) {
        show_folder(target.absoluteFilePath(), true);
        m_name->clear();
        return;
    }

    if (!target.dir().exists()) {
        m_footer_note->setText(QStringLiteral("The folder %1 does not exist").arg(target.absolutePath()));
        return;
    }

    if (target.exists() && m_replace_path != target.absoluteFilePath()) {
        ask_to_replace(target.absoluteFilePath());
        return;
    }

    file_encoding encoding = { m_encoding->currentData().toByteArray(), m_encoding->currentData(BOM_ROLE).toBool() };
    if (encoding.name != "UTF-8") {
        encoding.has_bom = encoding.name.startsWith("UTF-");
    }

    m_save = { target.absoluteFilePath(), encoding, m_eol->currentData().toInt() };
    finish(1);
}

void file_sheet::refresh_primary() {
    if (m_mode == file_sheet_mode::OPEN) {
        m_primary->setEnabled(!selected_files().isEmpty());
        return;
    }

    bool is_replacing = !m_replace_path.isEmpty();
    m_primary->setText(is_replacing ? QStringLiteral("Replace") : QStringLiteral("Save"));
    m_primary->setEnabled(!m_name->text().trimmed().isEmpty());
    QString replaced = QFileInfo(m_replace_path).fileName();
    m_footer_note->setText(is_replacing ? QStringLiteral("%1 exists here. Replace it?").arg(replaced) : QString());
}

// Asking inline keeps the sheet in place, and Esc returns to the name
void file_sheet::ask_to_replace(const QString& path) {
    m_replace_path = path;
    refresh_primary();
    m_primary->setFocus();
}

void file_sheet::set_title(const QString& title) {
    m_title->setText(title);
}

void file_sheet::set_note(const QString& note) {
    m_note->setText(note);
}

// Open selects the current file, or else the first row, so Enter opens it. Save As only points at the first row, so
// the name typed stays
void file_sheet::point_at_start() {
    QModelIndex first = m_model->index(0, 0);
    if (m_mode != file_sheet_mode::OPEN) {
        m_list->selectionModel()->setCurrentIndex(first, QItemSelectionModel::NoUpdate);
        return;
    }

    QModelIndex start = first;
    for (int row = 0; row < m_model->rowCount(QModelIndex()); row++) {
        if (m_model->entry(row).absoluteFilePath() == m_current_path) {
            start = m_model->index(row, 0);
        }
    }

    m_list->setCurrentIndex(start);
    m_list->scrollTo(start);
}

QString file_sheet::current_folder() const {
    return m_path->property("folder").toString();
}

QStringList file_sheet::selected_files() const {
    QStringList files;
    for (const QModelIndex& index : m_list->selectionModel()->selectedRows()) {
        const QFileInfo& entry = m_model->entry(index.row());
        if (entry.isFile()) {
            files.append(entry.absoluteFilePath());
        }
    }

    return files;
}

QString refusal_by_kind(const QString& path) {
    struct stat status = {};
    if (::stat(QFile::encodeName(path).constData(), &status) != 0 || S_ISREG(status.st_mode)) {
        return QString();
    }

    if (S_ISCHR(status.st_mode) || S_ISBLK(status.st_mode)) {
        return QStringLiteral("it is a device, not a file");
    }

    if (S_ISFIFO(status.st_mode)) {
        return QStringLiteral("it is a pipe, not a file");
    }

    return S_ISSOCK(status.st_mode) ? QStringLiteral("it is a socket, not a file")
                                    : QStringLiteral("it is not a regular file");
}
