/* uidemo: the first toolkit consumer, exercising labels, buttons,
 * checkboxes, flex layout, and resize reflow on a live desktop.
 */
#include <stlxui/stlxui.h>

#include <stlxwin/stlxwin.h>

#include <cstdio>

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);

    ui::app app("uidemo");
    if (!app.ok()) {
        printf("uidemo: no display manager\r\n");
        return 1;
    }

    ui::window_host* win = app.create_window(360, 240, "UI Demo",
                                             STLXWIN_WF_RESIZABLE);
    if (!win) {
        printf("uidemo: window creation failed\r\n");
        return 1;
    }

    auto root = std::make_unique<ui::box>(ui::axis::column);
    root->s().background = ui::theme::active().window_bg;
    root->s().padding = ui::edge_insets::all(16);
    root->s().gap = 12;

    root->add<ui::label>("Toolkit demo");

    ui::label* counter = root->add<ui::label>("Clicks: 0");
    counter->set_color(ui::theme::active().text_dim);

    ui::box* row = root->add<ui::box>(ui::axis::row);
    row->s().main = ui::length::content();
    row->s().gap = 8;

    ui::button* clicker = row->add<ui::button>("Click me");
    clicker->s().main = ui::length::content();
    int clicks = 0;
    clicker->on_click = [&clicks, counter]() {
        clicks++;
        char text[32];
        snprintf(text, sizeof(text), "Clicks: %d", clicks);
        counter->set_text(text);
        printf("uidemo: click %d\r\n", clicks);
    };

    ui::button* quit = row->add<ui::button>("Quit");
    quit->s().main = ui::length::content();
    quit->on_click = [&app]() {
        printf("uidemo: quit\r\n");
        app.quit(0);
    };

    ui::checkbox* opt = root->add<ui::checkbox>("Enable the thing", false);
    opt->on_change = [](bool v) {
        printf("uidemo: option %s\r\n", v ? "on" : "off");
    };

    /* A flex spacer pushes the footer to the bottom edge */
    ui::box* spacer = root->add<ui::box>();
    spacer->s().main = ui::length::flex();

    ui::label* footer = root->add<ui::label>("Resize me");
    footer->set_color(ui::theme::active().text_dim);

    win->set_root(std::move(root));

    printf("uidemo: up\r\n");
    return app.run();
}
