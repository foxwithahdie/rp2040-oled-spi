/*
 * SPDX-License-Identifier: MIT
 * Copyright 2023, Artem Savkov
 */

#include <string.h>

#include "i2c.h"

// starts the i2c peripheral at the configured baudrate and hands the sda/scl
// pins over to it. the internal pull ups are turned on too
void rp2040_i2c_init(rp2040_oled_t *oled) {
  i2c_init(oled->i2c, oled->baudrate);

  gpio_set_function(oled->sda_pin, GPIO_FUNC_I2C);
  gpio_set_function(oled->scl_pin, GPIO_FUNC_I2C);

  gpio_pull_up(oled->sda_pin);
  gpio_pull_up(oled->scl_pin);
}

// checks if anything answers at `addr` by trying to read a byte from it.
// if nothing acks the read fails with PICO_ERROR_GENERIC
bool rp2040_i2c_test_addr(rp2040_oled_t *oled, uint8_t addr) {
  uint8_t buf;
  int ret;

  ret = i2c_read_blocking(oled->i2c, addr, &buf, 1, false);
  return ret != PICO_ERROR_GENERIC;
}

// writes the register number, then reads `len` bytes back into `data`.
// returns the number of bytes read or a negative pico error code
int rp2040_i2c_read_register(rp2040_oled_t *oled, uint8_t reg, uint8_t *data,
                             size_t len) {
  int ret;

  // nostop = true so the read right after is a repeated start
  ret = i2c_write_blocking(oled->i2c, oled->addr, &reg, 1, true);
  if (ret < 0)
    return ret;

  ret = i2c_read_blocking(oled->i2c, oled->addr, data, len, false);
  return ret;
}

// sends `data` to the display. data[0] has to be a control byte
// (0x00 for commands, OLED_CB_DATA_BIT for pixel data).
// anything 32 bytes or longer gets split into 32 byte i2c transfers. every
// chunk after the first gets a fresh OLED_CB_DATA_BIT control byte in front,
// so it carries 31 bytes of the original data.
// returns how many bytes of `data` were sent, or -1 on a failed write
size_t rp2040_i2c_write(rp2040_oled_t *oled, const uint8_t *data, size_t len) {
  uint8_t buf[32];
  size_t sent = 0;
  size_t ret = 0;
  uint8_t leftover;
  // send full 32 byte chunks while there's enough left
  while (len - sent >= 32) {
    if (sent == 0) {
      // first chunk already starts with the caller's control byte
      memcpy(buf, data, 32);
      data += 32;
      sent += 32;
    } else {
      // later chunks: 1 control byte + 31 bytes of data
      buf[0] = OLED_CB_DATA_BIT;
      memcpy(buf + 1, data, 31);
      data += 31;
      sent += 31;
    }
    ret = i2c_write_blocking(oled->i2c, oled->addr, buf, 32, true);
  }

  leftover = len - sent;
  // nothing was chunked (len < 32), so just send the whole thing as is
  if (leftover == len) {
    ret = i2c_write_blocking(oled->i2c, oled->addr, data, len, true);
    return ret == len ? ret : -1;
  }
  // send whatever didn't fill a full chunk, with its own control byte
  if (leftover > 0) {
    buf[0] = OLED_CB_DATA_BIT;
    memcpy(buf + 1, data, leftover);
    ret = i2c_write_blocking(oled->i2c, oled->addr, buf, leftover + 1, true);
    if (ret != (leftover + 1))
      return -1;

    sent += leftover;
  }

  return sent;
}
