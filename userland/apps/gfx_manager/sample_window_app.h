#ifndef SAMPLE_WINDOW_APP_H
#define SAMPLE_WINDOW_APP_H
#include "window.h"

class sample_window_app {
public:
    bool init();
    void render();

private:
    window* m_window;
    kstl::shared_ptr<canvas> m_canvas;
};

#endif // SAMPLE_WINDOW_APP_H
