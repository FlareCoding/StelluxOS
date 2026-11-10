#ifndef STLXDM_SHELL_BAR_HPP
#define STLXDM_SHELL_BAR_HPP

#include "draw.hpp"
#include "stats.hpp"

#include <stlxgfx/surface.h>
#include <stlxui/stlxui.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

class bar_separator;

/* One of the bar's click targets. Hover goes to the widget under the pointer, so a target has no
 * children and paints its hover and open states and content. Its frame spans the bar's height and
 * reaches `reach_left` past its hover background, so a press at the output's edge still hits it. */
class bar_target : public ui::widget {
public:
    explicit bar_target(int32_t reach_left);
    ~bar_target() override;

    /* Fired by a press on the target */
    std::function<void()> on_press;

    void set_open(bool open);

    void paint(ui::painter& p) override;
    void on_pointer_enter() override;
    void on_pointer_leave() override;
    bool on_pointer_down(const ui::pointer_event& e) override;

protected:
    /* The hover background's rect, which the content lies in */
    ui::rect slot() const;

private:
    int32_t            m_reach_left;
    bool               m_hover = false;
    bool               m_open = false;
    stlxgfx_surface_t* m_ring = nullptr;
};

/* The Stellux mark at the bar's left, which opens the system menu */
class mark_target : public bar_target {
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
    /* How far the mark's hover background and the readouts' frame lie in from the output's sides */
    static constexpr int32_t TARGET_INSET = 8;

    /* Builds the bar's tree over `glass`, a picture of its background that
     * must outlive the tree. The mark and the readouts are widgets of that
     * tree and the `show_` calls change its widgets, so neither may be used
     * before `build` or after the tree is destroyed. */
    std::unique_ptr<ui::widget> build(const stlxgfx_surface_t* glass);

    mark_target* mark() const { return m_mark; }
    readouts_target* readouts() const { return m_readouts; }

    /* Shows `name` beside the mark, nothing when it is empty */
    void show_app_name(const std::string& name);

    /* Shows the UTC date and time to the minute */
    void show_clock();

private:
    mark_target*     m_mark = nullptr;
    bar_separator*   m_separator = nullptr;
    ui::label*       m_app_name = nullptr;
    ui::label*       m_date = nullptr;
    ui::label*       m_time = nullptr;
    readouts_target* m_readouts = nullptr;
};

#endif
