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

    ui::text_input* field = root->add<ui::text_input>();
    field->set_placeholder("type and press enter");
    field->on_submit = [](const std::string& t) {
        printf("uidemo: submit '%s'\r\n", t.c_str());
    };

    /* A scrolling list takes the leftover height */
    ui::scroll_view* list = root->add<ui::scroll_view>();
    list->s().main = ui::length::flex();

    ui::box* items = list->add<ui::box>(ui::axis::column);
    items->s().main = ui::length::content();
    items->s().gap = 4;
    for (int i = 1; i <= 12; i++) {
        char text[24];
        snprintf(text, sizeof(text), "List item %d", i);
        items->add<ui::label>(text);
    }

    ui::label* footer = root->add<ui::label>("Resize me");
    footer->set_color(ui::theme::active().text_dim);

    win->set_root(std::move(root));

    printf("uidemo: up\r\n");
    return app.run();
}
