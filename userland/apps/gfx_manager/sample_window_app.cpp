#include "sample_window_app.h"

example_window::example_window() {
    title = "Example Window";
    window_size.width = 400;
    window_size.height = 300;
    position.x = 80;
    position.y = 80;
    background_color = stella_ui::color(0xff4f4d49);
}

void example_window::draw() {
    m_canvas->clear();
    m_canvas->draw_string(20, 20, "This is an example app label\n", 0xffc8e8e0);
}

