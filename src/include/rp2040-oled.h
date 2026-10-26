/*
 * SPDX-License-Identifier: MIT
 * Copyright 2023, Artem Savkov
 */

#ifndef _RP2040_OLED_H
#define _RP2040_OLED_H

#include "hardware/i2c.h"
#include "pico/stdlib.h"

// sentinel for "not set". used for reset_pin and addr (addr gets scanned
// for if it's left as this or 0x00)
#define PIN_UNDEF 0xff

#define GPIO_LEVEL_HIGH 1
#define GPIO_LEVEL_LOW 0
// the display ram is split into horizontal "pages" that are 8 pixels tall.
// one byte = one column of 8 vertical pixels in a page, bit 0 is the top
#define PAGE_BITS 8

// i2c control byte. every i2c write to the display starts with one of these
// to say whether the bytes after it are commands or pixel data.
// 0x00 = commands follow
// 0x40 = gdram (pixel) data follows
// 0x80 = continuation: only one byte follows, then another control byte
enum {
  OLED_CB_CONTINUATION_BIT = 0x80,
  OLED_CB_DATA_BIT = 0x40,
};

// supported panel sizes (width x height in pixels)
typedef enum {
  OLED_128x128 = 1,
  OLED_128x64,
  OLED_128x32,
  OLED_132x64,
  OLED_96x16,
  OLED_64x128,
  OLED_64x32,
  OLED_72x40,
} rp2040_oled_size_t;

// how drawn bytes get combined with what's already in the framebuffer
typedef enum {
  OLED_COLOR_BLACK = 0, // AND: 0 bits turn pixels off
  OLED_COLOR_WHITE,     // OR: 1 bits turn pixels on
  OLED_COLOR_FULL_BYTE, // overwrite the whole byte as-is
} rp2040_oled_color_t;

// controller chip + i2c address that init detected. each chip has a _3C
// and _3D entry right after each other so the address can just be added on
typedef enum {
  OLED_NOT_FOUND = -1,
  OLED_SSD1306_3C,
  OLED_SSD1306_3D,
  OLED_SH1106_3C,
  OLED_SH1106_3D,
  OLED_SH1107_3C,
  OLED_SH1107_3D,
} rp2040_oled_type_t;

// command bytes for the ssd1306 / sh1106 / sh1107 controllers. some of
// these get OR'd with a value (like PAGE_ADDR | page) and some take an
// argument byte right after them
typedef enum {
  OLED_CMD_SET_LC_ADDR = 0x00, // | low nibble of the column address
  OLED_CMD_SET_HC_ADDR = 0x10, // | high nibble of the column address
  OLED_CMD_SET_SSD1306_ADDR_MODE = 0x20, // arg: 0x02 = page addressing
  OLED_CMD_SET_ADDR_PAGE = 0x20,         // sh1107: page addressing mode
  OLED_CMD_SET_ADDR_VERTICAL = 0x21,     // sh1107: vertical addressing mode
  OLED_CMD_SET_DISPLAY_STARTLINE0 = 0x40, // | start line, here line 0
  OLED_CMD_SET_CONTRAST = 0x81,           // arg: contrast 0x00 - 0xff
  OLED_CMD_SET_CHARGE_PUMP = 0x8d,        // ssd1306. arg: 0x14 = on
  OLED_CMD_SET_SEGMENT_REMAP_NORMAL = 0xa0,  // column 0 on the left
  OLED_CMD_SET_SEGMENT_REMAP_REVERSE = 0xa1, // columns mirrored
  OLED_CMD_SET_DISPLAY_RAM = 0xa4,     // show what's in ram
  OLED_CMD_SET_DISPLAY_ENTIRE = 0xa5,  // light every pixel, ignore ram
  OLED_CMD_SET_DISPLAY_NORMAL = 0xa6,  // 1 bit = lit pixel
  OLED_CMD_SET_DISPLAY_INVERSE = 0xa7, // 0 bit = lit pixel
  OLED_CMD_SET_MULTIPLEX_RATIO = 0xa8, // arg: number of rows - 1
  OLED_CMD_SET_DC_DC = 0xad,           // sh110x dc-dc setting
  OLED_CMD_DISPLAY_OFF = 0xae,
  OLED_CMD_DISPLAY_ON = 0xaf,
  OLED_CMD_SET_PAGE_ADDR = 0xb0,        // | page number to write to
  OLED_CMD_SET_SCAN_DIR_NORMAL = 0xc0,  // rows scanned top to bottom
  OLED_CMD_SET_SCAN_DIR_REVERSE = 0xc8, // rows mirrored
  OLED_CMD_SET_DISPLAY_OFFSET = 0xd3,   // arg: vertical shift
  OLED_CMD_SET_DISPLAY_CLOCK = 0xd5,    // arg: clock divide / osc freq
  OLED_CMD_SET_PRECHARGE_PERIOD = 0xd9, // arg: precharge period
  OLED_CMD_SET_COM_PINS = 0xda,         // arg: com pin hardware layout
  OLED_CMD_SET_VCOM_DESELECT_LEVEL = 0xdb, // arg: vcomh level
  OLED_CMD_SET_DISPLAY_STARTLINE = 0xdc,   // sh1107. arg: start line
  OLED_CMD_RMW_START = 0xe0, // start read-modify-write mode (sh110x)
  OLED_CMD_RMW_END = 0xee,   // end read-modify-write mode
} rp2040_oled_cmd_t;

// mirror the image. the init sequences already set the "reverse" scan and
// remap, so flipping sets them back to "normal"
typedef enum {
  FLIP_NONE = 0x0,
  FLIP_HORIZONTAL = 0x1,
  FLIP_VERTICAL = 0x2,
  FLIP_BOTH = (FLIP_HORIZONTAL | FLIP_VERTICAL)
} rp2040_oled_flip_t;

// one of these per display. the caller fills in the "config" fields and
// rp2040_oled_init fills in the rest
typedef struct _rp2040_oled {
  // config: set these before calling rp2040_oled_init
  i2c_inst_t *i2c; // i2c0 or i2c1
  uint8_t sda_pin;
  uint8_t scl_pin;
  uint32_t baudrate;
  uint8_t addr;      // 0x00 or PIN_UNDEF = scan for it
  uint8_t reset_pin; // PIN_UNDEF = no reset pin wired up
  rp2040_oled_size_t size;
  uint8_t width;  // filled in from size by init
  uint8_t height; // filled in from size by init
  bool invert;
  rp2040_oled_flip_t flip;

  // driver state: managed by the library
  // gdram = local copy of the display's ram (width * height / 8 bytes).
  // with doublebuf this is "what's on the screen right now"
  uint8_t *gdram;
  size_t gdram_size;
  // where the next write_gdram lands. x = column, y = page (not pixel row)
  struct {
    uint8_t x;
    uint8_t y;
  } cursor;
  // doublebuf on: a second full framebuffer that drawing goes into
  // doublebuf off: a bitmap with one bit per column per page, set when that
  // column has changed and needs to be sent on the next flush
  uint8_t *dirty_buf;
  size_t dirty_buf_size;
  bool is_dirty; // something changed since the last flush
  bool use_doublebuf;
} rp2040_oled_t;

#ifdef __cplusplus
extern "C" {
#endif

// most of the drawing functions take a `render` flag.
// render = false: only update the framebuffer in memory, call
// rp2040_oled_flush later to send everything at once.
// render = true: send the change to the display right away.

// sets up i2c, resets the display, finds its address, figures out which
// controller it is, and sends the init sequence. returns the detected
// controller type or OLED_NOT_FOUND
rp2040_oled_type_t rp2040_oled_init(rp2040_oled_t *oled);
// clears the framebuffer and the screen
bool rp2040_oled_clear(rp2040_oled_t *oled);
// clears only the framebuffer. the screen updates on the next flush
bool rp2040_oled_clear_gdram(rp2040_oled_t *oled);
// sets screen brightness, 0x00 - 0xff
bool rp2040_oled_set_contrast(rp2040_oled_t *oled, uint8_t contrast);
// turns the panel on or off. ram contents are kept while it's off
bool rp2040_oled_set_power(rp2040_oled_t *oled, bool enabled);
// draws `len` characters of `msg` with the built in 6x8 font. y gets
// rounded down to a multiple of 8, so text always sits on a page
bool rp2040_oled_write_string(rp2040_oled_t *oled, uint8_t x, uint8_t y,
                              char *msg, size_t len, bool render);
// turns one pixel on (WHITE) or off (BLACK)
bool rp2040_oled_set_pixel(rp2040_oled_t *oled, uint8_t x, uint8_t y,
                           rp2040_oled_color_t color, bool render);
// draws a sprite stored in the display's own format: `width` bytes per
// 8 pixel tall page, each byte is a column with bit 0 on top. the sprite
// can hang off any edge of the screen and gets clipped
bool rp2040_oled_draw_sprite(rp2040_oled_t *oled, const uint8_t *sprite,
                             int16_t x, int16_t y, uint8_t width,
                             uint8_t height, rp2040_oled_color_t color,
                             bool render);
// same as draw_sprite but takes a normal row by row 1 bit per pixel image
// (msb = leftmost pixel). pitch = bytes per row in the source image
bool rp2040_oled_draw_sprite_pitched(rp2040_oled_t *oled, uint8_t *sprite,
                                     int16_t x, int16_t y, uint8_t width,
                                     uint8_t height, uint8_t pitch,
                                     rp2040_oled_color_t color, bool render);
// draws a line from (x0, y0) to (x1, y1). both ends have to be on screen
bool rp2040_oled_draw_line(rp2040_oled_t *oled, uint8_t x0, uint8_t y0,
                           uint8_t x1, uint8_t y1, rp2040_oled_color_t color,
                           bool render);
// draws a rectangle between two corners, either an outline or filled
bool rp2040_oled_draw_rectangle(rp2040_oled_t *oled, uint8_t x0, uint8_t y0,
                                uint8_t x1, uint8_t y1,
                                rp2040_oled_color_t color, bool fill,
                                bool render);
// draws a circle centered at (x, y) with radius r
bool rp2040_oled_draw_circle(rp2040_oled_t *oled, int16_t x, int16_t y,
                             uint8_t r, rp2040_oled_color_t color, bool fill,
                             bool render);
// draws an ellipse centered at (x, y) with x radius rx and y radius ry
bool rp2040_oled_draw_ellipse(rp2040_oled_t *oled, int16_t x, int16_t y,
                              uint8_t rx, uint8_t ry, rp2040_oled_color_t color,
                              bool fill, bool render);
// sends everything that changed since the last flush to the display
bool rp2040_oled_flush(rp2040_oled_t *oled);

#ifdef __cplusplus
}
#endif
#endif /* _RP2040_OLED_H */
