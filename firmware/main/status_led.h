/* Status LED + setup button handling. */
#pragma once

#include <stdbool.h>

/* True if the setup button is held right now (used at boot). */
bool button_is_pressed(void);

/* Starts the LED pattern task and the long-press watcher (5 s -> setup portal). */
void status_led_start(void);
