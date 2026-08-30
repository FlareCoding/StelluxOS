/* Host tree ownership. Painting, damage, and input dispatch land
 * with the host core, layout lives in layout.cpp.
 */
#include <stlxui/stlxui.h>

namespace ui {

void host::set_root(std::unique_ptr<widget> root) {
    m_focus = nullptr;
    m_hover = nullptr;
    m_pointer_grab = nullptr;
    m_root = std::move(root);

    if (!m_root) {
        return;
    }

    /* The whole tree binds to this host */
    std::vector<widget*> stack = { m_root.get() };
    while (!stack.empty()) {
        widget* w = stack.back();
        stack.pop_back();
        w->m_host = this;

        for (auto& c : w->m_children) {
            stack.push_back(c.get());
        }
    }

    m_root->invalidate_layout();
}

} // namespace ui
