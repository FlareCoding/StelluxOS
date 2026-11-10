#include "kit.hpp"
#include "settings.hpp"

#include <QtMath>

constexpr int MIN_REPEAT_DELAY_MS = 150;
constexpr int MAX_REPEAT_DELAY_MS = 1000;
constexpr int REPEAT_DELAY_STEP_MS = 10;
constexpr int MIN_KEYS_PER_SECOND = 5;
constexpr int MAX_KEYS_PER_SECOND = 50;
constexpr int SLIDER_TICK_COUNT = 8;
constexpr qreal MS_PER_SECOND = 1000;

constexpr const char* KEYBOARD_CARD = "Keyboard";
constexpr const char* REPEAT_LABEL = "Repeat held keys";
constexpr const char* DELAY_LABEL = "Delay before repeat";
constexpr const char* DELAY_KEY = "key_repeat_delay_ms";
constexpr const char* RATE_LABEL = "Repeat rate";
constexpr const char* INTERVAL_KEY = "key_repeat_interval_ms";

/* The delay turning repeat on writes, since turning repeat off writes 0: the last nonzero delay the page read or the
 * slider set since Settings started, or libstlxconf's default before either. */
static uint32_t g_repeat_delay_ms = STLXCONF_DEFAULT_KEY_REPEAT_DELAY_MS;

/* The file stores the time between repeats, shown as keys per second within the slider's range: 40 ms is 25. An
 * interval of 0 shows the default rate, which turning repeat on writes. */
static int keys_per_second(uint32_t interval_ms) {
    uint32_t shown_interval_ms = interval_ms == 0 ? STLXCONF_DEFAULT_KEY_REPEAT_INTERVAL_MS : interval_ms;
    return qBound(MIN_KEYS_PER_SECOND, qRound(MS_PER_SECOND / shown_interval_ms), MAX_KEYS_PER_SECOND);
}

/* The delay the slider shows, kept to its range: the configured delay, or while that is 0 the delay turning
 * repeat on writes */
static int shown_delay_ms() {
    uint32_t delay_ms = g_conf.key_repeat_delay_ms != 0 ? g_conf.key_repeat_delay_ms : g_repeat_delay_ms;
    return qBound(MIN_REPEAT_DELAY_MS, static_cast<int>(delay_ms), MAX_REPEAT_DELAY_MS);
}

static QString repeat_key() {
    return QStringLiteral("%1 > 0 and %2 > 0").arg(QLatin1String(DELAY_KEY), QLatin1String(INTERVAL_KEY));
}

static QString delay_key() {
    return QStringLiteral("%1 = %2").arg(QLatin1String(DELAY_KEY)).arg(g_conf.key_repeat_delay_ms);
}

static QString rate_key() {
    return QStringLiteral("%1 = %2").arg(QLatin1String(INTERVAL_KEY)).arg(g_conf.key_repeat_interval_ms);
}

void build_input_page(QWidget* page) {
    if (g_conf.key_repeat_delay_ms != 0) {
        g_repeat_delay_ms = g_conf.key_repeat_delay_ms;
    }

    card* keyboard = add_section(page, QLatin1String(KEYBOARD_CARD), QStringLiteral("[input]"));
    bool is_repeating = is_key_repeat_on(g_conf);

    auto* repeat_row = new setting_row(QLatin1String(REPEAT_LABEL), repeat_key());
    auto* repeat_switch = new toggle(is_repeating);
    repeat_row->add_control(repeat_switch);
    keyboard->add_row(repeat_row);

    int shown_delay = shown_delay_ms();
    auto* delay_slider = new slider(
        MIN_REPEAT_DELAY_MS,
        MAX_REPEAT_DELAY_MS,
        REPEAT_DELAY_STEP_MS,
        shown_delay,
        SLIDER_TICK_COUNT
    );
    auto* delay_value = new value_label(QString::number(shown_delay), QStringLiteral("ms"));

    auto* delay_row = new setting_row(QLatin1String(DELAY_LABEL), delay_key());
    delay_row->add_control(delay_slider);
    delay_row->add_control(delay_value);
    keyboard->add_row(delay_row);

    int shown_rate = keys_per_second(g_conf.key_repeat_interval_ms);
    auto* rate_slider = new slider(MIN_KEYS_PER_SECOND, MAX_KEYS_PER_SECOND, 1, shown_rate, SLIDER_TICK_COUNT);
    auto* rate_value = new value_label(QString::number(shown_rate), QStringLiteral("/s"));

    auto* rate_row = new setting_row(QLatin1String(RATE_LABEL), rate_key());
    rate_row->add_control(rate_slider);
    rate_row->add_control(rate_value);
    keyboard->add_row(rate_row);

    auto enable_sliders = [delay_slider, delay_value, rate_slider, rate_value](bool is_on) {
        delay_slider->setEnabled(is_on);
        delay_value->setEnabled(is_on);
        rate_slider->setEnabled(is_on);
        rate_value->setEnabled(is_on);
    };

    enable_sliders(is_repeating);

    repeat_switch->on_toggle = [enable_sliders, delay_row, rate_row](bool is_on) {
        g_conf.key_repeat_delay_ms = is_on ? g_repeat_delay_ms : 0;
        if (is_on && g_conf.key_repeat_interval_ms == 0) {
            g_conf.key_repeat_interval_ms = STLXCONF_DEFAULT_KEY_REPEAT_INTERVAL_MS;
        }

        enable_sliders(is_on);
        delay_row->set_key(delay_key());
        rate_row->set_key(rate_key());
        show_pending_changes();
    };

    delay_slider->on_change = [delay_value, delay_row](int delay_ms) {
        g_repeat_delay_ms = static_cast<uint32_t>(delay_ms);
        g_conf.key_repeat_delay_ms = g_repeat_delay_ms;
        delay_value->set_value(QString::number(delay_ms));
        delay_row->set_key(delay_key());
        show_pending_changes();
    };

    /* The file holds whole milliseconds, so the knob shows the rate the stored interval gives */
    rate_slider->on_change = [rate_slider, rate_value, rate_row](int rate) {
        uint32_t interval_ms = static_cast<uint32_t>(qRound(MS_PER_SECOND / rate));
        int stored_rate = keys_per_second(interval_ms);

        g_conf.key_repeat_interval_ms = interval_ms;
        rate_slider->set_value(stored_rate);
        rate_value->set_value(QString::number(stored_rate));
        rate_row->set_key(rate_key());
        show_pending_changes();
    };
}

void add_input_entries(QList<search_entry>& entries) {
    bool is_repeating = is_key_repeat_on(g_conf);
    QString keyboard = QLatin1String(KEYBOARD_CARD);
    QString repeat = is_repeating ? QStringLiteral("On") : QStringLiteral("Off");
    QString delay = QStringLiteral("%1 ms").arg(shown_delay_ms());
    QString rate = QStringLiteral("%1 /s").arg(keys_per_second(g_conf.key_repeat_interval_ms));

    /* Each of these rows takes its label as its object name */
    QString repeat_label = QLatin1String(REPEAT_LABEL);
    QString delay_label = QLatin1String(DELAY_LABEL);
    QString rate_label = QLatin1String(RATE_LABEL);

    entries.append({ INPUT_PAGE, keyboard, repeat_label, repeat_key(), repeat, repeat_label });
    entries.append({ INPUT_PAGE, keyboard, delay_label, QLatin1String(DELAY_KEY), delay, delay_label });
    entries.append({ INPUT_PAGE, keyboard, rate_label, QLatin1String(INTERVAL_KEY), rate, rate_label });
}
