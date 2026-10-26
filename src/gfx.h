/*
 * SPDX-License-Identifier: MIT
 * Copyright 2023, Artem Savkov
 */

#include "include/rp2040-oled.h"

// internal: sends the whole framebuffer to the display, dirty or not.
// rp2040-oled.c uses it to clear the screen during init
bool rp2040_oled_force_flush(rp2040_oled_t *oled);
