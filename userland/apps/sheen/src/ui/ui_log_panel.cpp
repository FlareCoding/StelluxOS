#include "ui/ui_log_panel.h"
#include "core/kiln.h"
#include "ui/chrome.h"
#include "ui/ui_kit.h"

#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QShowEvent>
#include <QTimer>
#include <QVBoxLayout>

#include <Scintilla.h>
#include <ScintillaEditBase.h>

namespace sheen::ui {

static constexpr float LOG_FONT_PX = 13.0f;
static constexpr int LOG_LINE_HEIGHT = 18;
static constexpr int LOG_TEXT_MARGIN_X = 12;
static constexpr int INPUT_BAR_HEIGHT = 34;
static constexpr int INPUT_BAR_PADDING_X = 10;
static constexpr int INPUT_BAR_GAP = 8;
static constexpr int FOLLOW_PILL_MARGIN = 12;
static constexpr int STYLE_MARK_INFO = 40;
static constexpr int STYLE_MARK_DANGER = 41;
static constexpr int MARKER_MARK_INFO = 20;
static constexpr int MARKER_MARK_DANGER = 21;
static constexpr int LOG_LINK_INDICATOR = INDICATOR_CONTAINER + 5;
static constexpr uint32_t MARK_INFO_TINT = 0x14FFF0D7;
static constexpr uint32_t MARK_DANGER_TINT = 0x24B4483E;
static constexpr uint32_t LINK_COLOR = 0xFF8FBFAE;
static constexpr int MAX_UTF8_TAIL_BYTES = 3;

static const QRegularExpression LOCATION_PATTERN(
    QStringLiteral("([A-Za-z0-9_./+-]+\\.[A-Za-z0-9_+]+):(\\d+)(?::\\d+)?"));

namespace {

class input_bar_container : public QWidget {
public:
    explicit input_bar_container(QWidget* parent) : QWidget(parent) {
        setFixedHeight(INPUT_BAR_HEIGHT);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), token(STLXTHEME_OPAQUE_POCKET));
        painter.fillRect(QRect(0, 0, width(), 1), token(STLXTHEME_LINE));
    }
};

} // namespace

static QByteArray line_ending_bytes(const QString& label) {
    if (label.compare(QLatin1String("CRLF"), Qt::CaseInsensitive) == 0) {
        return QByteArrayLiteral("\r\n");
    }

    if (label.compare(QLatin1String("CR"), Qt::CaseInsensitive) == 0) {
        return QByteArrayLiteral("\r");
    }

    if (label.compare(QLatin1String("None"), Qt::CaseInsensitive) == 0) {
        return {};
    }

    return QByteArrayLiteral("\n");
}

static qsizetype complete_utf8_prefix_length(QByteArrayView bytes) {
    qsizetype size = bytes.size();
    qsizetype scan_limit = qMin<qsizetype>(size, MAX_UTF8_TAIL_BYTES);
    for (qsizetype back = 1; back <= scan_limit; back++) {
        auto byte = static_cast<unsigned char>(bytes[size - back]);
        if ((byte & 0xC0u) == 0x80u) {
            continue;
        }

        int expected_len = 1;
        if (byte >= 0xC2u && byte <= 0xDFu) {
            expected_len = 2;
        } else if (byte >= 0xE0u && byte <= 0xEFu) {
            expected_len = 3;
        } else if (byte >= 0xF0u && byte <= 0xF4u) {
            expected_len = 4;
        }

        if (expected_len > 1 && back < expected_len) {
            return size - back;
        }

        return size;
    }

    return size;
}

ui_log_panel::ui_log_panel(const log_panel_options& options, QWidget* parent)
    : QWidget(parent), m_options(options), m_view(new ScintillaEditBase(this)), m_flush_timer(new QTimer(this)) {
    m_flush_timer->setSingleShot(true);
    m_flush_timer->setInterval(qMax(1, m_options.flush_interval_ms));
    connect(m_flush_timer, &QTimer::timeout, this, [this] { flush_pending_bytes(false); });

    configure_scintilla();

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_view, 1);

    m_jump_to_bottom = new ui_pill_button(QStringLiteral("Jump to Bottom"), m_view->viewport());
    m_jump_to_bottom->hide();
    connect(m_jump_to_bottom, &QAbstractButton::clicked, this, [this] {
        m_is_following = true;
        m_view->send(SCI_SCROLLTOEND);
        update_follow_pill();
    });

    if (m_options.has_command_input) {
        auto* bar = new input_bar_container(this);
        auto* bar_layout = new QHBoxLayout(bar);
        bar_layout->setContentsMargins(INPUT_BAR_PADDING_X, 1, INPUT_BAR_PADDING_X, 0);
        bar_layout->setSpacing(INPUT_BAR_GAP);

        m_input = new QLineEdit(bar);
        m_input->setFont(mono_font(LOG_FONT_PX));
        m_input->setFrame(false);
        m_input->setPlaceholderText(m_options.input_placeholder);
        m_input->setStyleSheet(QStringLiteral("QLineEdit { background: transparent; color: %1; border: none; }")
                                   .arg(token(STLXTHEME_TEXT).name()));

        connect(m_input, &QLineEdit::returnPressed, this, [this] { submit_input(); });
        bar_layout->addWidget(m_input, 1);

        if (m_options.line_endings.size() > 1) {
            m_ending_button = new ui_pill_button(m_options.line_endings.first(), bar);
            connect(m_ending_button, &QAbstractButton::clicked, this, [this] {
                m_ending_index = (m_ending_index + 1) % static_cast<int>(m_options.line_endings.size());
                m_ending_button->setText(m_options.line_endings.at(m_ending_index));
                m_ending_button->updateGeometry();
            });
            bar_layout->addWidget(m_ending_button, 0, Qt::AlignVCenter);
        }

        layout->addWidget(bar);
    }

    m_view->viewport()->setMouseTracking(true);
    m_view->viewport()->installEventFilter(this);
    connect(m_view, &ScintillaEditBase::updateUi, this, [this](Scintilla::Update updated) {
        if (!m_suppress_scroll_check && (static_cast<int>(updated) & SC_UPDATE_V_SCROLL)) {
            update_follow_pill();
        }
    });
}

void ui_log_panel::configure_scintilla() {
    apply_text_area_look(m_view);
    set_line_numbers_shown(m_view, false);
    set_folding_shown(m_view, false);

    QByteArray family = QByteArrayLiteral(STLXTHEME_MONO_FONT_FAMILY);
    int size = static_cast<int>(LOG_FONT_PX * POINTS_PER_PIXEL * SC_FONT_SIZE_MULTIPLIER + 0.5);
    m_view->send(SCI_STYLESETFONT, STYLE_DEFAULT, reinterpret_cast<sptr_t>(family.constData()));
    m_view->send(SCI_STYLESETSIZEFRACTIONAL, STYLE_DEFAULT, size);

    m_view->send(SCI_STYLESETFORE, STYLE_DEFAULT, scintilla_rgb(STLXTHEME_TERMINAL_TEXT));
    m_view->send(SCI_STYLESETBACK, STYLE_DEFAULT, scintilla_rgb(STLXTHEME_OPAQUE_POCKET));
    m_view->send(SCI_STYLECLEARALL);

    m_view->send(SCI_STYLESETFORE, STYLE_MARK_INFO, scintilla_rgb(STLXTHEME_TEXT_DIM));
    m_view->send(SCI_STYLESETBACK, STYLE_MARK_INFO, scintilla_rgb(STLXTHEME_OPAQUE_POCKET));
    m_view->send(SCI_STYLESETFORE, STYLE_MARK_DANGER, scintilla_rgb(STLXTHEME_DANGER));
    m_view->send(SCI_STYLESETBACK, STYLE_MARK_DANGER, scintilla_rgb(STLXTHEME_OPAQUE_POCKET));

    define_line_tint(m_view, MARKER_MARK_INFO, MARK_INFO_TINT);
    define_line_tint(m_view, MARKER_MARK_DANGER, MARK_DANGER_TINT);

    m_view->send(SCI_SETEXTRAASCENT, 0);
    m_view->send(SCI_SETEXTRADESCENT, 0);
    int extra = qMax(0, LOG_LINE_HEIGHT - static_cast<int>(m_view->send(SCI_TEXTHEIGHT, 0)));
    m_view->send(SCI_SETEXTRAASCENT, extra - extra / 2);
    m_view->send(SCI_SETEXTRADESCENT, extra / 2);

    m_view->send(SCI_SETMARGINS, 1);
    m_view->send(SCI_SETMARGINTYPEN, 0, SC_MARGIN_COLOUR);
    m_view->send(SCI_SETMARGINWIDTHN, 0, 0);
    m_view->send(SCI_SETMARGINMASKN, 0, (1 << MARKER_MARK_INFO) | (1 << MARKER_MARK_DANGER));
    m_view->send(SCI_SETMARGINLEFT, 0, LOG_TEXT_MARGIN_X);
    m_view->send(SCI_SETMARGINRIGHT, 0, LOG_TEXT_MARGIN_X);

    m_view->send(SCI_SETINDENTATIONGUIDES, SC_IV_NONE);
    m_view->send(SCI_SETEDGEMODE, EDGE_NONE);
    m_view->send(SCI_SETCARETLINEVISIBLE, 0);
    m_view->send(SCI_SETCARETLINEVISIBLEALWAYS, 0);

    m_view->send(SCI_SETCARETSTYLE, CARETSTYLE_INVISIBLE);
    m_view->send(SCI_SETUNDOCOLLECTION, 0);
    m_view->send(SCI_SETCHANGEHISTORY, SC_CHANGE_HISTORY_DISABLED);

    m_view->send(SCI_INDICSETSTYLE, LOG_LINK_INDICATOR, INDIC_PLAIN);
    m_view->send(SCI_INDICSETFORE, LOG_LINK_INDICATOR, scintilla_rgb(LINK_COLOR));
    m_view->send(SCI_SETREADONLY, 1);
}

void ui_log_panel::append_bytes(QByteArrayView utf8) {
    if (utf8.isEmpty()) {
        return;
    }

    m_held_incomplete_utf8 = false;
    m_pending_bytes.append(utf8.data(), utf8.size());
    if (!m_flush_timer->isActive()) {
        m_flush_timer->start();
    }
}

void ui_log_panel::append_mark(const QString& caption, bool is_danger) {
    if (m_flush_timer->isActive()) {
        m_flush_timer->stop();
    }

    flush_pending_bytes(true);

    bool was_following = m_is_following;
    m_suppress_scroll_check = true;
    m_view->send(SCI_SETREADONLY, 0);
    sptr_t length = m_view->send(SCI_GETLENGTH);
    if (length > 0 && m_view->send(SCI_GETCHARAT, length - 1) != '\n') {
        m_view->send(SCI_APPENDTEXT, 1, reinterpret_cast<sptr_t>("\n"));
        length++;
    }

    sptr_t mark_line = m_view->send(SCI_LINEFROMPOSITION, length);
    QByteArray text = QStringLiteral("-- %1 --\n").arg(caption).toUtf8();
    m_view->send(SCI_APPENDTEXT, text.size(), reinterpret_cast<sptr_t>(text.constData()));
    m_view->send(SCI_STARTSTYLING, length, 0);
    m_view->send(SCI_SETSTYLING, text.size(), is_danger ? STYLE_MARK_DANGER : STYLE_MARK_INFO);

    m_view->send(SCI_MARKERADD, mark_line, is_danger ? MARKER_MARK_DANGER : MARKER_MARK_INFO);
    trim_ring_buffer();
    m_view->send(SCI_SETREADONLY, 1);

    if (was_following) {
        m_view->send(SCI_SCROLLTOEND);
    }

    m_suppress_scroll_check = false;
    m_is_following = was_following;
    update_follow_pill();
}

void ui_log_panel::clear_log() {
    m_flush_timer->stop();
    m_pending_bytes.clear();
    m_held_incomplete_utf8 = false;

    m_suppress_scroll_check = true;
    m_view->send(SCI_SETREADONLY, 0);
    m_view->send(SCI_CLEARALL);
    m_view->send(SCI_MARKERDELETEALL, -1);
    m_view->send(SCI_SETREADONLY, 1);
    m_suppress_scroll_check = false;

    m_is_following = true;
    if (m_jump_to_bottom) {
        m_jump_to_bottom->hide();
    }
}

void ui_log_panel::set_workspace_root(const QString& root_path) {
    m_workspace_root = root_path;
}

bool ui_log_panel::eventFilter(QObject* watched, QEvent* event) {
    if (watched != m_view->viewport()) {
        return QWidget::eventFilter(watched, event);
    }

    if (event->type() == QEvent::MouseMove) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        QPoint pos = mouse->position().toPoint();
        QString path;
        int line = 0;
        bool on_link = resolve_link_at(pos.x(), pos.y(), &path, &line);
        m_view->viewport()->setCursor(on_link ? Qt::PointingHandCursor : Qt::IBeamCursor);
    }

    if (event->type() == QEvent::MouseButtonRelease) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::LeftButton && m_view->send(SCI_GETSELECTIONEMPTY) && on_location_clicked) {
            QPoint pos = mouse->position().toPoint();
            QString path;
            int line = 0;
            if (resolve_link_at(pos.x(), pos.y(), &path, &line)) {
                on_location_clicked(path, line);
                return true;
            }
        }
    }

    return QWidget::eventFilter(watched, event);
}

void ui_log_panel::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    place_follow_pill();
}

void ui_log_panel::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (m_is_following) {
        m_suppress_scroll_check = true;
        m_view->send(SCI_SCROLLTOEND);
        m_suppress_scroll_check = false;
    }

    update_follow_pill();
}

void ui_log_panel::flush_pending_bytes(bool force_all) {
    if (m_pending_bytes.isEmpty()) {
        return;
    }

    qsizetype flush_len = (force_all || m_held_incomplete_utf8)
                        ? m_pending_bytes.size()
                        : complete_utf8_prefix_length(m_pending_bytes);
    if (flush_len == 0) {
        m_held_incomplete_utf8 = true;
        m_flush_timer->start();
        return;
    }

    QByteArray batch = m_pending_bytes.left(flush_len);
    m_pending_bytes.remove(0, flush_len);
    m_held_incomplete_utf8 = !m_pending_bytes.isEmpty();
    if (m_held_incomplete_utf8) {
        m_flush_timer->start();
    }

    bool was_following = m_is_following;
    m_suppress_scroll_check = true;
    m_view->send(SCI_SETREADONLY, 0);
    sptr_t first_touched_line = m_view->send(SCI_LINEFROMPOSITION, m_view->send(SCI_GETLENGTH));

    m_view->send(SCI_APPENDTEXT, batch.size(), reinterpret_cast<sptr_t>(batch.constData()));
    long dropped = trim_ring_buffer();
    mark_links_from_line(qMax<long>(0, static_cast<long>(first_touched_line) - dropped));
    m_view->send(SCI_SETREADONLY, 1);

    if (was_following) {
        m_view->send(SCI_SCROLLTOEND);
    }

    m_suppress_scroll_check = false;
    m_is_following = was_following;
    update_follow_pill();
}

long ui_log_panel::trim_ring_buffer() {
    if (m_options.max_lines <= 0) {
        return 0;
    }

    sptr_t line_count = m_view->send(SCI_GETLINECOUNT);
    if (line_count <= m_options.max_lines) {
        return 0;
    }

    sptr_t drop_lines = line_count - m_options.max_lines;
    sptr_t cut_pos = m_view->send(SCI_POSITIONFROMLINE, drop_lines);
    if (cut_pos > 0) {
        m_view->send(SCI_DELETERANGE, 0, cut_pos);
        return static_cast<long>(drop_lines);
    }

    return 0;
}

void ui_log_panel::mark_links_from_line(long first_line) {
    sptr_t line_count = m_view->send(SCI_GETLINECOUNT);
    m_view->send(SCI_SETINDICATORCURRENT, LOG_LINK_INDICATOR);

    for (sptr_t line = qMax<sptr_t>(0, first_line); line < line_count; line++) {
        sptr_t start = m_view->send(SCI_POSITIONFROMLINE, line);
        sptr_t end = m_view->send(SCI_GETLINEENDPOSITION, line);
        if (end <= start) {
            continue;
        }

        m_view->send(SCI_INDICATORCLEARRANGE, start, end - start);
        const char* raw = reinterpret_cast<const char*>(m_view->send(SCI_GETRANGEPOINTER, start, end - start));
        QString text = QString::fromUtf8(raw, end - start);
        QRegularExpressionMatchIterator it = LOCATION_PATTERN.globalMatch(text);
        while (it.hasNext()) {
            QRegularExpressionMatch match = it.next();
            if (resolve_existing_file(match.captured(1)).isEmpty()) {
                continue;
            }

            QByteArray prefix = text.left(match.capturedStart(0)).toUtf8();
            QByteArray span = match.captured(0).toUtf8();
            m_view->send(SCI_INDICATORFILLRANGE, start + prefix.size(), span.size());
        }
    }
}

bool ui_log_panel::resolve_link_at(int x, int y, QString* resolved_path, int* resolved_line) const {
    sptr_t pos = m_view->send(SCI_POSITIONFROMPOINTCLOSE, x, y);
    if (pos < 0 || !m_view->send(SCI_INDICATORVALUEAT, LOG_LINK_INDICATOR, pos)) {
        return false;
    }

    sptr_t line = m_view->send(SCI_LINEFROMPOSITION, pos);
    sptr_t start = m_view->send(SCI_POSITIONFROMLINE, line);
    sptr_t end = m_view->send(SCI_GETLINEENDPOSITION, line);
    const char* raw = reinterpret_cast<const char*>(m_view->send(SCI_GETRANGEPOINTER, start, end - start));
    QString text = QString::fromUtf8(raw, end - start);

    sptr_t byte_offset = pos - start;
    QRegularExpressionMatchIterator it = LOCATION_PATTERN.globalMatch(text);
    while (it.hasNext()) {
        QRegularExpressionMatch match = it.next();
        sptr_t match_start = text.left(match.capturedStart(0)).toUtf8().size();
        sptr_t match_end = match_start + match.captured(0).toUtf8().size();
        if (byte_offset < match_start || byte_offset > match_end) {
            continue;
        }

        QString existing = resolve_existing_file(match.captured(1));
        int target_line = match.captured(2).toInt();
        if (!existing.isEmpty() && target_line > 0) {
            *resolved_path = existing;
            *resolved_line = target_line;
            return true;
        }
    }

    return false;
}

QString ui_log_panel::resolve_existing_file(const QString& candidate) const {
    QFileInfo direct(candidate);
    if (direct.isAbsolute() && direct.isFile()) {
        return direct.absoluteFilePath();
    }

    if (!m_workspace_root.isEmpty()) {
        QFileInfo in_workspace(QDir(m_workspace_root).filePath(candidate));
        if (in_workspace.isFile()) {
            return in_workspace.absoluteFilePath();
        }
    }

    if (direct.isFile()) {
        return direct.absoluteFilePath();
    }

    return {};
}

void ui_log_panel::update_follow_pill() {
    if (m_suppress_scroll_check || !m_view->isVisible()) {
        return;
    }

    sptr_t lines_on_screen = m_view->send(SCI_LINESONSCREEN);
    if (lines_on_screen <= 0) {
        return;
    }

    sptr_t first_visible = m_view->send(SCI_GETFIRSTVISIBLELINE);
    sptr_t total_visible = m_view->send(SCI_VISIBLEFROMDOCLINE, qMax<sptr_t>(0, m_view->send(SCI_GETLINECOUNT) - 1)) + 1;
    bool at_bottom = total_visible <= lines_on_screen || first_visible + lines_on_screen >= total_visible - 1;
    m_is_following = at_bottom;

    if (m_jump_to_bottom) {
        m_jump_to_bottom->setVisible(!at_bottom);
        place_follow_pill();
    }
}

void ui_log_panel::place_follow_pill() {
    if (!m_jump_to_bottom || !m_jump_to_bottom->isVisible()) {
        return;
    }

    QSize hint = m_jump_to_bottom->sizeHint();
    QWidget* area = m_view->viewport();
    m_jump_to_bottom->setGeometry(area->width() - hint.width() - FOLLOW_PILL_MARGIN,
                                  area->height() - hint.height() - FOLLOW_PILL_MARGIN,
                                  hint.width(), hint.height());
    m_jump_to_bottom->raise();
}

void ui_log_panel::submit_input() {
    if (!m_input) {
        return;
    }

    QByteArray payload = m_input->text().toUtf8();
    QString ending = m_options.line_endings.isEmpty() ? QStringLiteral("LF")
                                                      : m_options.line_endings.value(m_ending_index);
    payload += line_ending_bytes(ending);
    m_input->clear();

    if (on_input_submitted) {
        on_input_submitted(payload);
    }
}

} // namespace sheen::ui
