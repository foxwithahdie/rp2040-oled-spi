/*
 * SPDX-License-Identifier: MIT
 * Copyright 2023, Artem Savkov
 */

#include <stdlib.h>
#include <string.h>

#include "display.h"
#include "gfx.h"
#include "i2c.h"

// sends a single command byte. 0x00 in front is the "commands follow"
// control byte
static bool rp2040_oled_write_command(rp2040_oled_t *oled, uint8_t cmd) {
  uint8_t buf[] = {0x00, cmd};
  return rp2040_i2c_write(oled, buf, sizeof(buf)) == sizeof(buf);
}

// sends a command that takes one argument byte (like contrast)
static bool rp2040_oled_write_command_with_arg(rp2040_oled_t *oled, uint8_t cmd,
                                               uint8_t arg) {
  uint8_t buf[] = {0x00, cmd, arg};
  return rp2040_i2c_write(oled, buf, sizeof(buf)) == sizeof(buf);
}

// hardware reset: hold the reset pin low for 50ms, release it, then give
// the controller 10ms to come back up
static void rp2040_oled_reset(rp2040_oled_t *oled) {
  gpio_put(oled->reset_pin, GPIO_LEVEL_LOW);
  sleep_ms(50);
  gpio_put(oled->reset_pin, GPIO_LEVEL_HIGH);
  sleep_ms(10);
}

// picks the width/height and init sequence for the panel size, sends it,
// applies invert/flip, then allocates the framebuffer and dirty buffer and
// blanks the screen. returns -1 if the size isn't supported
static int rp2040_oled_display_init(rp2040_oled_t *oled) {
  const uint8_t *initbuf;
  size_t initlen;

  // some sizes share an init sequence (see display.h)
  switch (oled->size) {
  case OLED_128x128:
    oled->width = 128;
    oled->height = 128;
    initbuf = oled128_initbuf;
    initlen = sizeof(oled128_initbuf);
    break;
  case OLED_128x64:
    oled->width = 128;
    oled->height = 64;
    initbuf = oled64_initbuf;
    initlen = sizeof(oled64_initbuf);
    break;
  case OLED_128x32:
    oled->width = 128;
    oled->height = 32;
    initbuf = oled32_initbuf;
    initlen = sizeof(oled32_initbuf);
    break;
  case OLED_132x64:
    oled->width = 132;
    oled->height = 64;
    initbuf = oled64_initbuf;
    initlen = sizeof(oled64_initbuf);
    break;
  case OLED_96x16:
    oled->width = 96;
    oled->height = 16;
    initbuf = oled32_initbuf;
    initlen = sizeof(oled32_initbuf);
    break;
  case OLED_72x40:
    oled->width = 72;
    oled->height = 40;
    initbuf = oled72_initbuf;
    initlen = sizeof(oled72_initbuf);
    break;
  case OLED_64x128:
    oled->width = 64;
    oled->height = 128;
    initbuf = oled64x128_initbuf;
    initlen = sizeof(oled64x128_initbuf);
    break;
  case OLED_64x32:
    oled->width = 64;
    oled->height = 32;
    initbuf = oled64_initbuf;
    initlen = sizeof(oled64_initbuf);
    break;
  default:
    return -1;
  };

  // init sequence starts with its own 0x00 control byte
  rp2040_i2c_write(oled, initbuf, initlen);

  if (oled->invert)
    rp2040_oled_write_command(oled, OLED_CMD_SET_DISPLAY_INVERSE);

  // the init sequences set remap/scan to "reverse", so setting them back to
  // "normal" is what mirrors the image.
  // note: this is an else if, so FLIP_BOTH only does the horizontal flip
  if (oled->flip & FLIP_HORIZONTAL)
    rp2040_oled_write_command(oled, OLED_CMD_SET_SEGMENT_REMAP_NORMAL);
  else if (oled->flip & FLIP_VERTICAL)
    rp2040_oled_write_command(oled, OLED_CMD_SET_SCAN_DIR_NORMAL);

  // one byte per column per page, so 8 pixels per byte
  oled->gdram_size = oled->width * oled->height / PAGE_BITS;
  oled->gdram = malloc(oled->gdram_size);
  memset(oled->gdram, 0x00, oled->gdram_size);

  // doublebuf: dirty_buf is a full second framebuffer.
  // otherwise: dirty_buf is 1 bit per column per page
  if (oled->use_doublebuf) {
    oled->dirty_buf_size = oled->gdram_size;
  } else {
    oled->dirty_buf_size = (oled->width / 8) * (oled->height / PAGE_BITS);
  }

  // push the all zero gdram out so the screen starts blank instead of
  // showing whatever garbage was in the display's ram.
  // note: dirty_buf isn't allocated yet at this point
  rp2040_oled_force_flush(oled);

  oled->dirty_buf = malloc(oled->dirty_buf_size);
  memset(oled->dirty_buf, 0x00, oled->dirty_buf_size);

  return 0;
}

// tries each address in scan_addrs (0x3c, 0x3d) and returns the first one
// that answers, or PIN_UNDEF if nothing does
static uint8_t rp2040_oled_scan(rp2040_oled_t *oled) {
  uint8_t i;
  uint8_t addr = 0x00;

  for (i = 0; i < sizeof(scan_addrs); i++) {
    addr = scan_addrs[i];
    if (rp2040_i2c_test_addr(oled, addr))
      return addr;
  }

  return PIN_UNDEF;
}

// tells an sh1106 apart from an ssd1306, since they report the same status.
// the sh1106 has a read-modify-write mode and lets you read its ram back,
// the ssd1306 doesn't. the idea: turn the panel off, then for each test byte
// enter rmw mode, check that the byte written last loop came back, write
// the next test byte, and leave rmw mode. if every byte makes it through
// it's an sh1106.
// note: nothing is actually read into buf here, so buf[1] is still
// OLED_CMD_RMW_START when it gets compared
bool rp2040_oled_is_sh1106(rp2040_oled_t *oled) {
  const uint8_t TEST_DATA[] = {0xf2, 0x3a, 0x45, 0x8b, 0x00};
  uint8_t buf[4];
  uint8_t i;

  rp2040_oled_set_power(oled, false);

  for (i = 0; i < sizeof(TEST_DATA); i++) {
    // 0x80 = one command byte follows (RMW_START),
    // 0xc0 = one data byte follows
    buf[0] = OLED_CB_CONTINUATION_BIT;
    buf[1] = OLED_CMD_RMW_START;
    buf[2] = 0xc0;
    if (rp2040_i2c_write(oled, buf, 3) != 3)
      break;

    // compare against the byte written on the previous loop
    if (i > 0 && buf[1] != TEST_DATA[i - 1])
      break;

    // write one data byte, then one command byte (RMW_END)
    buf[0] = 0xc0;
    buf[1] = TEST_DATA[i];
    buf[2] = OLED_CB_CONTINUATION_BIT;
    buf[3] = OLED_CMD_RMW_END;

    if (rp2040_i2c_write(oled, buf, 4) != 4)
      break;
  }

  rp2040_oled_set_power(oled, true);

  // only true if the loop never broke out early
  return i == sizeof(TEST_DATA);
}

// reads the controller's status byte and uses its low nibble to guess which
// chip it is. 128x128 panels reporting 0x07/0x0f are sh1107, 0x08 is sh1106,
// and 0x03/0x06/0x07 could be either an ssd1306 or sh1106 so it runs the
// rmw test above
rp2040_oled_type_t rp2040_oled_autodetect(rp2040_oled_t *oled) {
  rp2040_oled_type_t type = OLED_NOT_FOUND;
  uint8_t status = 0x00;

  if (rp2040_i2c_read_register(oled, 0x00, &status, 1) < 0)
    return OLED_NOT_FOUND;

  status &= 0x0f;

  if ((status == 0x07 || status == 0x0f) && oled->size == OLED_128x128) {
    // sh1107 is mounted the other way round, so toggle the flip setting
    oled->flip = !oled->flip;
    type = OLED_SH1107_3C;
  } else if (status == 0x08) {
    type = OLED_SH1106_3C;
  } else if (status == 0x03 || status == 0x06 || status == 0x07) {
    if (rp2040_oled_is_sh1106(oled)) {
      type = OLED_SH1106_3C;
    } else {
      type = OLED_SSD1306_3C;
    }
  }

  // every _3D entry is right after its _3C one in the enum
  if (type != OLED_NOT_FOUND && oled->addr == 0x3d)
    type++;

  return type;
}

// entry point for the library. order matters here: autodetect can change
// oled->flip, so it has to run before display_init applies it
rp2040_oled_type_t rp2040_oled_init(rp2040_oled_t *oled) {
  rp2040_oled_type_t type = OLED_NOT_FOUND;
  rp2040_i2c_init(oled);

  if (oled->reset_pin != PIN_UNDEF) {
    gpio_set_dir(oled->reset_pin, GPIO_OUT);
    rp2040_oled_reset(oled);
  }

  // no address given: scan for one. address given: make sure it answers
  if (oled->addr == PIN_UNDEF || oled->addr == 0x00) {
    oled->addr = rp2040_oled_scan(oled);
    if (oled->addr == PIN_UNDEF)
      return OLED_NOT_FOUND;
  } else if (!rp2040_i2c_test_addr(oled, oled->addr)) {
    return OLED_NOT_FOUND;
  }

  type = rp2040_oled_autodetect(oled);

  rp2040_oled_display_init(oled);

  return type;
}

// brightness, 0x00 (dimmest) to 0xff (brightest)
bool rp2040_oled_set_contrast(rp2040_oled_t *oled, uint8_t contrast) {
  return rp2040_oled_write_command_with_arg(oled, OLED_CMD_SET_CONTRAST,
                                            contrast);
}

// display on/off command. the display keeps its ram while off
bool rp2040_oled_set_power(rp2040_oled_t *oled, bool enabled) {
  return rp2040_oled_write_command(oled, enabled ? OLED_CMD_DISPLAY_ON
                                                 : OLED_CMD_DISPLAY_OFF);
}
