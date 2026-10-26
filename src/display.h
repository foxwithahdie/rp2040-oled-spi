/*
 * SPDX-License-Identifier: MIT
 * Copyright 2023, Artem Savkov
 */

// different display settings
// each *_initbuf is a list of commands sent to the display once at startup.
// the first byte is the 0x00 "commands follow" control byte, after that it's
// commands, some followed by their argument byte. rp2040_oled_display_init
// picks one based on oled->size

#include "include/rp2040-oled.h"

// i2c addresses these displays usually sit on, tried in order by the scan
const uint8_t scan_addrs[] = {0x3c, 0x3d};

// 128x128 (sh1107)
const uint8_t oled128_initbuf[] = {0x00,
                                   OLED_CMD_DISPLAY_OFF,
                                   OLED_CMD_SET_DISPLAY_STARTLINE,
                                   0x00, // start at line 0
                                   OLED_CMD_SET_CONTRAST,
                                   0x40,
                                   OLED_CMD_SET_SEGMENT_REMAP_REVERSE,
                                   0xc8, // OLED_CMD_SET_SCAN_DIR_REVERSE
                                   OLED_CMD_SET_MULTIPLEX_RATIO,
                                   0x7f, // 128 rows
                                   OLED_CMD_SET_DISPLAY_CLOCK,
                                   0x50,
                                   OLED_CMD_SET_PRECHARGE_PERIOD,
                                   0x22,
                                   OLED_CMD_SET_VCOM_DESELECT_LEVEL,
                                   0x35,
                                   OLED_CMD_SET_PAGE_ADDR | 0x00, // page 0
                                   OLED_CMD_SET_COM_PINS,
                                   0x12,
                                   OLED_CMD_SET_DISPLAY_RAM,
                                   OLED_CMD_SET_DISPLAY_NORMAL,
                                   OLED_CMD_DISPLAY_ON};

// 64x128 tall panel (sh1107)
const uint8_t oled64x128_initbuf[] = {0x00,
                                      OLED_CMD_DISPLAY_OFF,
                                      OLED_CMD_SET_DISPLAY_CLOCK,
                                      0x51,
                                      OLED_CMD_SET_ADDR_PAGE,
                                      OLED_CMD_SET_MULTIPLEX_RATIO,
                                      0x3f, // 64 rows
                                      OLED_CMD_SET_DISPLAY_STARTLINE,
                                      0x00, // start at line 0
                                      OLED_CMD_SET_DISPLAY_OFFSET,
                                      0x60, // shift down 96 lines
                                      OLED_CMD_SET_DC_DC,
                                      0x80,
                                      OLED_CMD_SET_DISPLAY_NORMAL,
                                      OLED_CMD_SET_DISPLAY_RAM,
                                      OLED_CMD_SET_SEGMENT_REMAP_NORMAL,
                                      0xc0, // OLED_CMD_SET_SCAN_DIR_NORMAL
                                      OLED_CMD_SET_CONTRAST,
                                      0x40,
                                      OLED_CMD_SET_PRECHARGE_PERIOD,
                                      0x22,
                                      OLED_CMD_SET_VCOM_DESELECT_LEVEL,
                                      0x35,
                                      OLED_CMD_DISPLAY_ON};

// 132x64 (sh1106). starts the column address at 2 since the sh1106 has
// 132 columns of ram but most panels only show the middle 128.
// note: nothing references this one right now, OLED_132x64 uses
// oled64_initbuf
const uint8_t oled132_initbuf[] = {0x00,
                                   OLED_CMD_DISPLAY_OFF,
                                   OLED_CMD_SET_LC_ADDR | 0x02,
                                   OLED_CMD_SET_HC_ADDR | 0x00,
                                   OLED_CMD_SET_DISPLAY_STARTLINE0,
                                   OLED_CMD_SET_CONTRAST,
                                   0xa0,
                                   OLED_CMD_SET_SCAN_DIR_NORMAL,
                                   OLED_CMD_SET_DISPLAY_NORMAL,
                                   OLED_CMD_SET_MULTIPLEX_RATIO,
                                   0x3f, // 64 rows
                                   OLED_CMD_SET_DISPLAY_OFFSET,
                                   0x00, // no shift
                                   OLED_CMD_SET_DISPLAY_CLOCK,
                                   0x80,
                                   OLED_CMD_SET_PRECHARGE_PERIOD,
                                   0xf1,
                                   OLED_CMD_SET_COM_PINS,
                                   0x12,
                                   OLED_CMD_SET_VCOM_DESELECT_LEVEL,
                                   0x40,
                                   OLED_CMD_SET_SSD1306_ADDR_MODE,
                                   0x02, // page addressing
                                   OLED_CMD_SET_DISPLAY_RAM,
                                   OLED_CMD_SET_DISPLAY_NORMAL};

// 128x64 (ssd1306). also used for 132x64 and 64x32
const uint8_t oled64_initbuf[] = {0x00,
                                  OLED_CMD_DISPLAY_OFF,
                                  OLED_CMD_SET_MULTIPLEX_RATIO,
                                  0x3f, // 64 rows
                                  OLED_CMD_SET_DISPLAY_OFFSET,
                                  0x00, // no shift
                                  OLED_CMD_SET_DISPLAY_STARTLINE0,
                                  OLED_CMD_SET_SEGMENT_REMAP_REVERSE,
                                  0xc8, // OLED_CMD_SET_SCAN_DIR_REVERSE
                                  OLED_CMD_SET_COM_PINS,
                                  0x12, // alternative com pin layout
                                  OLED_CMD_SET_CONTRAST,
                                  0xff,
                                  OLED_CMD_SET_DISPLAY_RAM,
                                  OLED_CMD_SET_DISPLAY_NORMAL,
                                  OLED_CMD_SET_DISPLAY_CLOCK,
                                  0x80,
                                  OLED_CMD_SET_CHARGE_PUMP,
                                  0x14, // charge pump on
                                  OLED_CMD_DISPLAY_ON,
                                  OLED_CMD_SET_SSD1306_ADDR_MODE,
                                  0x02}; // page addressing

// 128x32 (ssd1306). also used for 96x16
const uint8_t oled32_initbuf[] = {0x00,
                                  OLED_CMD_DISPLAY_OFF,
                                  OLED_CMD_SET_DISPLAY_CLOCK,
                                  0x80,
                                  OLED_CMD_SET_MULTIPLEX_RATIO,
                                  0x1f, // 32 rows
                                  OLED_CMD_SET_DISPLAY_OFFSET,
                                  0x00, // no shift
                                  OLED_CMD_SET_DISPLAY_STARTLINE0,
                                  OLED_CMD_SET_CHARGE_PUMP,
                                  0x14, // charge pump on
                                  OLED_CMD_SET_SEGMENT_REMAP_REVERSE,
                                  0xc8, // OLED_CMD_SET_SCAN_DIR_REVERSE
                                  OLED_CMD_SET_COM_PINS,
                                  0x02, // sequential com pin layout
                                  OLED_CMD_SET_CONTRAST,
                                  0x7f,
                                  OLED_CMD_SET_PRECHARGE_PERIOD,
                                  0xf1,
                                  OLED_CMD_SET_VCOM_DESELECT_LEVEL,
                                  0x40,
                                  OLED_CMD_SET_DISPLAY_RAM,
                                  OLED_CMD_SET_DISPLAY_NORMAL,
                                  OLED_CMD_DISPLAY_ON};

// 72x40 (ssd1306 variant)
const uint8_t oled72_initbuf[] = {0x00,
                                  OLED_CMD_DISPLAY_OFF,
                                  OLED_CMD_SET_MULTIPLEX_RATIO,
                                  0x3f, // 64 rows
                                  OLED_CMD_SET_DISPLAY_OFFSET,
                                  0x00, // no shift
                                  OLED_CMD_SET_DISPLAY_STARTLINE0,
                                  OLED_CMD_SET_SEGMENT_REMAP_REVERSE,
                                  0xc8, // OLED_CMD_SET_SCAN_DIR_REVERSE
                                  OLED_CMD_SET_COM_PINS,
                                  0x12, // alternative com pin layout
                                  OLED_CMD_SET_CONTRAST,
                                  0xff,
                                  OLED_CMD_SET_DC_DC,
                                  0x30, // on ssd1306 0xad is the iref setting
                                  OLED_CMD_SET_PRECHARGE_PERIOD,
                                  0xf1,
                                  OLED_CMD_SET_DISPLAY_RAM,
                                  OLED_CMD_SET_DISPLAY_NORMAL,
                                  OLED_CMD_SET_DISPLAY_CLOCK,
                                  0x80,
                                  OLED_CMD_SET_CHARGE_PUMP,
                                  0x14, // charge pump on
                                  OLED_CMD_DISPLAY_ON,
                                  OLED_CMD_SET_SSD1306_ADDR_MODE,
                                  0x02}; // page addressing
