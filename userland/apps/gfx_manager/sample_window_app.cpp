#include "sample_window_app.h"

bool sample_window_app::init() {
    m_window = window::create_window(300, 200, "Example App");
    if (!m_window) {
        return false;
    }

    m_canvas = m_window->get_canvas();
    m_canvas->set_background_color(0xff4f4d49);

    return true;
}

void sample_window_app::render() {
    m_canvas->clear();
    m_canvas->draw_string(20, 20, "This is an example app label\n", 0xffc8e8e0);
}
