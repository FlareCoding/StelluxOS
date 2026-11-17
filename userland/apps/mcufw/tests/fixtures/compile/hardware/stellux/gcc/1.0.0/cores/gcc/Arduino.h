#ifndef ARDUINO_H
#define ARDUINO_H

#ifdef __cplusplus
extern "C" {
#endif

void print_line(const char* text);
unsigned long board_millis(void);

#ifdef __cplusplus
}
#endif

void setup();
void loop();
void init_variant();

#endif
