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

/* The delay turning repeat on writes, since turning repeat off writes 0: the last nonzero delay the page read or the
 * slider set since Settings started, or libstlxconf's default before either. */
static uint32_t g_repeat_delay_ms = STLXCONF_DEFAULT_KEY_REPEAT_DELAY_MS;

/* The file stores the time between repeats, shown as keys per second within the slider's range: 40 ms is 25. An
 * interval of 0 shows the default rate, which turning repeat on writes. */
static int keys_per_second(uint32_t interval_ms) {
    uint32_t shown_interval_ms = interval_ms == 0 ? STLXCONF_DEFAULT_KEY_REPEAT_INTERVAL_MS : interval_ms;
    return qBound(MIN_KEYS_PER_SECOND, qRound(MS_PER_SECOND / shown_interval_ms), MAX_KEYS_PER_SECOND);
}

static QString delay_key() {
    return QStringLiteral("key_repeat_delay_ms = %1").arg(g_conf.key_repeat_delay_ms);
}

static QString rate_key() {
    return QStringLiteral("key_repeat_interval_ms = %1").arg(g_conf.key_repeat_interval_ms);
}

void build_input_page(QWidget* page) {
    if (g_conf.key_repeat_delay_ms != 0) {
        g_repeat_delay_ms = g_conf.key_repeat_delay_ms;
    }

    card* keyboard = add_section(page, QStringLiteral("Keyboard"), QStringLiteral("[input]"));
    bool is_repeating = is_key_repeat_on(g_conf);

    auto* repeat_row = new setting_row(
        QStringLiteral("Repeat held keys"),
        QStringLiteral("key_repeat_delay_ms > 0 and key_repeat_interval_ms > 0")
    );
    auto* repeat_switch = new toggle(is_repeating);
    repeat_row->add_control(repeat_switch);
    keyboard->add_row(repeat_row);

    int shown_delay = qBound(MIN_REPEAT_DELAY_MS, static_cast<int>(g_repeat_delay_ms), MAX_REPEAT_DELAY_MS);
    auto* delay_slider = new slider(
        MIN_REPEAT_DELAY_MS,
        MAX_REPEAT_DELAY_MS,
        REPEAT_DELAY_STEP_MS,
        shown_delay,
        SLIDER_TICK_COUNT
    );
    auto* delay_value = new value_label(QString::number(shown_delay), QStringLiteral("ms"));

    auto* delay_row = new setting_row(QStringLiteral("Delay before repeat"), delay_key());
    delay_row->add_control(delay_slider);
    delay_row->add_control(delay_value);
    keyboard->add_row(delay_row);

    int shown_rate = keys_per_second(g_conf.key_repeat_interval_ms);
    auto* rate_slider = new slider(MIN_KEYS_PER_SECOND, MAX_KEYS_PER_SECOND, 1, shown_rate, SLIDER_TICK_COUNT);
    auto* rate_value = new value_label(QString::number(shown_rate), QStringLiteral("/s"));

    auto* rate_row = new setting_row(QStringLiteral("Repeat rate"), rate_key());
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
