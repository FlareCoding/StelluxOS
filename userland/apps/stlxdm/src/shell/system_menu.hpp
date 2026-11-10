#ifndef STLXDM_SHELL_SYSTEM_MENU_HPP
#define STLXDM_SHELL_SYSTEM_MENU_HPP

#include "draw.hpp"
#include "popover.hpp"
#include "stats.hpp"

#include <stlxui/stlxui.h>

#include <cstdint>
#include <functional>
#include <memory>

/* The menu under the Stellux mark: the release and the uptime, then Settings and the task monitor, and Power,
 * which opens the power overlay */
class system_menu : public popover_panel {
public:
    explicit system_menu(const stats_sampler& stats) : m_stats(stats) {}

    /* Fired by a chosen item: a program to spawn with its args, or the power overlay. Both must be set before the
     * menu opens. */
    std::function<void(const char* path, const char* args)> on_spawn;
    std::function<void()> on_power;

    int32_t width() const override;
    int32_t left() const override;
    std::unique_ptr<ui::widget> build() override;
    void refresh() override;
    void closed() override;

private:
    /* The sampler whose uptime the header shows */
    const stats_sampler& m_stats;

    /* The header's uptime label, from `build()` until `closed()` */
    mono_label* m_uptime = nullptr;
};

#endif
