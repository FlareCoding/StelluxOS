#ifndef STLXDM_SPLASH_HPP
#define STLXDM_SPLASH_HPP

class presenter;

/**
 * @brief Runs the boot splash until Enter is pressed.
 *
 * A flying starfield with a pulsing title, drawn through the
 * presenter at a paced frame rate. Reads the keyboard directly,
 * returning once the user confirms, and leaves the device untouched
 * for the input layer that opens it afterwards.
 */
void splash_run(presenter& pres);

#endif
