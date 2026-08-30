/* The process global theme: one palette every widget consults,
 * replaced as a whole so a reload repaints everything consistently.
 */
#include <stlxui/stlxui.h>

namespace ui {

static theme g_theme;

const theme& theme::active() {
    return g_theme;
}

void theme::set_active(const theme& t) {
    g_theme = t;
}

} // namespace ui
