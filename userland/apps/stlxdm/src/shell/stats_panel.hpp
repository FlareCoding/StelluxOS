#ifndef STLXDM_SHELL_STATS_PANEL_HPP
#define STLXDM_SHELL_STATS_PANEL_HPP

#include "popover.hpp"
#include "stats.hpp"

#include <stlxui/stlxui.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

/* The default interface as last read for the bar: its IPv4 address or the reason it has none, and its name,
 * empty without an interface */
struct network_summary {
    std::string address;
    std::string interface;
};

/* The panel under the readouts. It shows each CPU's load, the overall load
 * over the last minute, memory, uptime, tasks, the network address and the
 * busiest processes, and repaints its figures with every sample. It takes no
 * keys, so its pin can keep it open while presses and keys go to the windows. */
class stats_panel : public popover_panel {
public:
    /* `stats` and `network` must outlive the panel */
    stats_panel(stats_sampler& stats, const network_summary& network) : m_stats(stats), m_network(network) {}

    /* Fired by the Task monitor and Settings links: a program to spawn with its args. It must be set before the
     * panel opens. */
    std::function<void(const char* path, const char* args)> on_spawn;

    int32_t width() const override;
    int32_t left(int32_t output_w) const override;
    std::unique_ptr<ui::widget> build() override;
    void refresh() override;
    void closed() override;
    bool pinned() const override { return m_pinned; }

private:
    stats_sampler&           m_stats;
    const network_summary&   m_network;
    bool                     m_pinned = false;

    /* The widgets that paint figures, repainted with every sample */
    std::vector<ui::widget*> m_live;
};

#endif
