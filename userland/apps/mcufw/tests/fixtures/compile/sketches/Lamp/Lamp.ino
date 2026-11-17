#include "helpers.h"
#include "src/effects/fade.h"

void set_color(int color);

void setup() {
    print_line("lamp starting");
    set_color(3);
}

void loop() {
    print_line(fade_label(helper_level()));
}
