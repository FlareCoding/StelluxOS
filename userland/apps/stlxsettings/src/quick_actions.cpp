#include "kit.hpp"
#include "settings.hpp"

#include <QtMath>

#include <iterator>

constexpr int MIN_HISTORY_ITEMS = 10;
constexpr int HISTORY_ITEMS_STEP = 10;
constexpr int MIN_HISTORY_MB = 8;
constexpr int HISTORY_MB_STEP = 8;
constexpr int SLIDER_TICK_COUNT = 8;

constexpr const char* QUICK_ACTIONS_CARD = "Quick Actions";
constexpr const char* SKIP_LABEL = "Folders file search skips";
constexpr const char* SKIP_KEY = "skip";

constexpr const char* CLIPBOARD_CARD = "Clipboard history";
constexpr const char* ITEMS_LABEL = "Copies kept";
constexpr const char* ITEMS_KEY = "history_items";
constexpr const char* SIZE_LABEL = "Memory for copies";
constexpr const char* SIZE_KEY = "history_mb";

constexpr const char* CAPTURE_CARD = "Capture bar";
constexpr const char* TIMER_LABEL = "Timer";
constexpr const char* TIMER_KEY = "timer";
constexpr const char* SHADOW_LABEL = "Window shadow";
constexpr const char* SHADOW_KEY = "shadow";
constexpr const char* POINTER_LABEL = "Show pointer";
constexpr const char* POINTER_KEY = "pointer";
constexpr const char* REMEMBER_LABEL = "Remember last region";
constexpr const char* REMEMBER_KEY = "remember_region";

/* Words a search finds each card's rows by beyond their labels and keys */
constexpr const char* QUICK_ACTIONS_WORDS = "palette files";
constexpr const char* CLIPBOARD_WORDS = "clipboard copies history";
constexpr const char* CAPTURE_WORDS = "capture screenshot";

/* The timers the capture bar offers, which the file stores in seconds */
static const uint32_t TIMER_SECONDS[] = { 0, 5, 10 };
static const char* const TIMER_CHOICES[] = { "None", "5 s", "10 s" };
static_assert(std::size(TIMER_SECONDS) == std::size(TIMER_CHOICES));

static QString keyed(const char* key, const QString& value) {
    return QStringLiteral("%1 = %2").arg(QLatin1String(key), value);
}

static QString switch_text(bool is_on) {
    return is_on ? QStringLiteral("on") : QStringLiteral("off");
}

static int timer_choice(uint32_t seconds) {
    for (int i = 0; i < static_cast<int>(std::size(TIMER_SECONDS)); i++) {
        if (TIMER_SECONDS[i] == seconds) {
            return i;
        }
    }

    return 0;
}

/* A slider over a whole number the file stores, its value beside it and its key under the label kept current */
static void add_number_row(card* section, const char* label, const char* key, uint32_t* value, int minimum,
                           int maximum, int step, const QString& unit) {
    int shown = qBound(minimum, static_cast<int>(*value), maximum);
    auto* number = new slider(minimum, maximum, step, shown, SLIDER_TICK_COUNT);
    auto* number_value = new value_label(QString::number(shown), unit);

    auto* row = new setting_row(QLatin1String(label), keyed(key, QString::number(*value)));
    row->add_control(number);
    row->add_control(number_value);
    section->add_row(row);

    number->on_change = [value, number_value, row, key](int chosen) {
        *value = static_cast<uint32_t>(chosen);
        number_value->set_value(QString::number(chosen));
        row->set_key(keyed(key, QString::number(chosen)));
        show_pending_changes();
    };
}

/* A switch over an option the file stores as on or off */
static void add_switch_row(card* section, const char* label, const char* key, bool* value) {
    auto* row = new setting_row(QLatin1String(label), keyed(key, switch_text(*value)));
    auto* control = new toggle(*value);
    row->add_control(control);
    section->add_row(row);

    control->on_toggle = [value, row, key](bool is_on) {
        *value = is_on;
        row->set_key(keyed(key, switch_text(is_on)));
        show_pending_changes();
    };
}

void build_quick_actions_page(QWidget* page) {
    card* palette = add_section(page, QLatin1String(QUICK_ACTIONS_CARD), QStringLiteral("[quick_actions]"));
    palette->add_row(make_field_row(QLatin1String(SKIP_LABEL), QLatin1String(SKIP_KEY), g_conf.quick_actions_skip,
                                    sizeof(g_conf.quick_actions_skip), [] { show_pending_changes(); }));

    card* clipboard = add_section(page, QLatin1String(CLIPBOARD_CARD), QStringLiteral("[clipboard]"));
    add_number_row(clipboard, ITEMS_LABEL, ITEMS_KEY, &g_conf.clipboard_history_items, MIN_HISTORY_ITEMS,
                   STLXCONF_CLIPBOARD_ITEMS_MAX, HISTORY_ITEMS_STEP, QStringLiteral("copies"));
    add_number_row(clipboard, SIZE_LABEL, SIZE_KEY, &g_conf.clipboard_history_mb, MIN_HISTORY_MB,
                   STLXCONF_CLIPBOARD_MB_MAX, HISTORY_MB_STEP, QStringLiteral("MB"));

    card* capture = add_section(page, QLatin1String(CAPTURE_CARD), QStringLiteral("[screenshot]"));
    QStringList timers;
    for (const char* choice : TIMER_CHOICES) {
        timers.append(QLatin1String(choice));
    }

    auto* timer_row = new setting_row(QLatin1String(TIMER_LABEL),
                                      keyed(TIMER_KEY, QString::number(g_conf.screenshot_timer)));
    auto* timer = new segmented(timers, timer_choice(g_conf.screenshot_timer), nullptr);
    timer_row->add_control(timer);
    capture->add_row(timer_row);
    timer->on_choose = [timer_row](int chosen) {
        g_conf.screenshot_timer = TIMER_SECONDS[chosen];
        timer_row->set_key(keyed(TIMER_KEY, QString::number(g_conf.screenshot_timer)));
        show_pending_changes();
    };

    add_switch_row(capture, SHADOW_LABEL, SHADOW_KEY, &g_conf.screenshot_shadow);
    add_switch_row(capture, POINTER_LABEL, POINTER_KEY, &g_conf.screenshot_pointer);
    add_switch_row(capture, REMEMBER_LABEL, REMEMBER_KEY, &g_conf.screenshot_remember_region);
}

void add_quick_actions_entries(QList<search_entry>& entries) {
    QString palette = QLatin1String(QUICK_ACTIONS_CARD);
    QString clipboard = QLatin1String(CLIPBOARD_CARD);
    QString capture = QLatin1String(CAPTURE_CARD);
    QString timer = QLatin1String(TIMER_CHOICES[timer_choice(g_conf.screenshot_timer)]);

    /* Each of these rows takes its label as its object name */
    const struct {
        const QString& card;
        const char*    label;
        const char*    key;
        QString        value;
        const char*    keywords;
    } rows[] = {
        { palette, SKIP_LABEL, SKIP_KEY, QString::fromUtf8(g_conf.quick_actions_skip), QUICK_ACTIONS_WORDS },
        { clipboard, ITEMS_LABEL, ITEMS_KEY, QString::number(g_conf.clipboard_history_items), CLIPBOARD_WORDS },
        { clipboard, SIZE_LABEL, SIZE_KEY, QStringLiteral("%1 MB").arg(g_conf.clipboard_history_mb), CLIPBOARD_WORDS },
        { capture, TIMER_LABEL, TIMER_KEY, timer, CAPTURE_WORDS },
        { capture, SHADOW_LABEL, SHADOW_KEY, switch_text(g_conf.screenshot_shadow), CAPTURE_WORDS },
        { capture, POINTER_LABEL, POINTER_KEY, switch_text(g_conf.screenshot_pointer), CAPTURE_WORDS },
        { capture, REMEMBER_LABEL, REMEMBER_KEY, switch_text(g_conf.screenshot_remember_region), CAPTURE_WORDS },
    };

    for (const auto& row : rows) {
        QString label = QLatin1String(row.label);
        entries.append({ QUICK_ACTIONS_PAGE, row.card, label, QLatin1String(row.key), row.value, label,
                         QLatin1String(row.keywords) });
    }
}
