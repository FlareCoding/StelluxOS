#ifndef STLXDM_SHELL_BAR_HPP
#define STLXDM_SHELL_BAR_HPP

#include "draw.hpp"
#include "stats.hpp"

#include <stlxgfx/surface.h>
#include <stlxui/stlxui.h>

#include <cstdint>
#include <memory>
#include <string>

class bar_separator;

/* The Stellux mark at the bar's left */
class mark_target : public ui::widget {
public:
    mark_target();
    ~mark_target() override;

    ui::size measure(ui::size avail) override;
    void paint(ui::painter& p) override;

private:
    stlxgfx_surface_t* m_mark = nullptr;
};

/* The CPU, memory and network readouts at the bar's right */
class readouts_target : public ui::widget {
public:
    readouts_target();

    void show_stats(const stats_sampler& stats);
    void show_network(const char* summary);

    ui::size measure(ui::size avail) override;
    void paint(ui::painter& p) override;

private:
    int32_t cpu_figure_width() const;
    int32_t paint_readout(ui::painter& p, int32_t x, const text_picture& label, const text_picture& figure) const;
    int32_t paint_separator(ui::painter& p, int32_t x) const;
    int32_t paint_meter(ui::painter& p, int32_t x) const;

    uint32_t     m_cpu_load = 0;
    text_picture m_cpu_label;
    text_picture m_memory_label;
    text_picture m_network_label;
    text_picture m_cpu;
    text_picture m_memory;
    text_picture m_network;
};

/* The top bar: the mark, the focused app's name, the clock centered on the
 * output, and the readouts, on glass with a hairline along its bottom */
class top_bar {
public:
    /* Builds the bar's tree over `glass`, a picture of its background that
     * must outlive the tree. The readouts are a widget of that tree and the
     * `show_` calls change its widgets, so neither may be used before
     * `build` or after the tree is destroyed. */
    std::unique_ptr<ui::widget> build(const stlxgfx_surface_t* glass);

    readouts_target* readouts() const { return m_readouts; }

    /* Shows `name` beside the mark, nothing when it is empty */
    void show_app_name(const std::string& name);

    /* Shows the UTC date and time to the minute */
    void show_clock();

private:
    bar_separator*   m_separator = nullptr;
    ui::label*       m_app_name = nullptr;
    ui::label*       m_date = nullptr;
    ui::label*       m_time = nullptr;
    readouts_target* m_readouts = nullptr;
};

#endif
