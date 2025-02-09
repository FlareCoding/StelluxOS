#ifndef SAMPLE_WINDOW_APP_H
#define SAMPLE_WINDOW_APP_H
#include <stella_ui.h>

class example_window : public stella_ui::window_base {
public:
    example_window();

    void draw() override;
};

#endif // SAMPLE_WINDOW_APP_H
