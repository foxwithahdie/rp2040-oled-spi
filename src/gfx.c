/*
 * SPDX-License-Identifier: MIT
 * Copyright 2023, Artem Savkov
 */

#include <stdlib.h>
#include <string.h>

#include "font.h"
#include "i2c.h"

// framebuffer layout, used everywhere in this file:
// the screen is split into pages that are 8 pixels tall. byte
// [x + page * width] is column x of that page, bit 0 = top pixel. so pixel
// (x, y) lives at byte x + (y / 8) * width, bit y % 8

// allocates a data buffer and moves it over to oled memory
// offset for access
// the buffer is really size + 1 bytes: byte 0 is the OLED_CB_DATA_BIT
// control byte, and the caller gets a pointer to byte 1. that way buf - 1
// can be handed straight to rp2040_i2c_write without copying
static uint8_t *rp2040_oled_alloc_data_buf(size_t size) {
  uint8_t *buf;

  buf = malloc(size + 1);
  memset(buf, 0x00, size + 1);
  buf[0] = OLED_CB_DATA_BIT;

  return buf + 1;
}

// unsets offset and frees
static void rp2040_oled_free_data_buf(uint8_t *buf) { free(buf - 1); }

// sets the position of a cursor. moves it based on screen size.
// x is a column and y is a pixel row, which gets turned into a page.
// smaller panels only use part of the controller's ram, so their offset
// into it gets added here. if render is true it also sends the page and
// column address commands so the display's own write pointer matches
static bool rp2040_oled_set_position(rp2040_oled_t *oled, uint8_t x, uint8_t y,
                                     bool render) {
  uint8_t buf[4];

  // pixel row -> page
  y /= PAGE_BITS;

  // where the visible area sits in the controller's ram for each size
  if (oled->size == OLED_64x32) {
    x += 32;
    if (oled->flip == 0)
      y += 4;
  } else if (oled->size == OLED_132x64) {
    x += 2;
  } else if (oled->size == OLED_96x16) {
    if (oled->flip == 0)
      y += 2;
    else
      x += 32;
  } else if (oled->size == OLED_72x40) {
    x += 28;
    if (oled->flip == 0)
      y += 3;
  }

  // note: the cursor holds the offset adjusted x and the page, not the
  // pixel row that was passed in
  oled->cursor.x = x;
  oled->cursor.y = y;

  if (!render)
    return true;

  // 0x00 control byte, then: which page, low column nibble, high nibble
  buf[0] = 0x00;
  buf[1] = OLED_CMD_SET_PAGE_ADDR | y;
  buf[2] = OLED_CMD_SET_LC_ADDR | (x & 0x0f);
  buf[3] = OLED_CMD_SET_HC_ADDR | (x >> 4);

  return rp2040_i2c_write(oled, buf, sizeof(buf)) == sizeof(buf);
}

// sends `size` bytes of page y, starting at column x, from gdram to the
// display. in doublebuf mode it first copies that span from dirty_buf
// (what was drawn) into gdram (what's on screen) so the two match again
static bool rp2040_oled_render_gdram(rp2040_oled_t *oled, uint8_t x, uint8_t y,
                                     size_t gdram_offset, uint8_t size) {
  uint8_t *buf = NULL;

  if (oled->use_doublebuf)
    memcpy(oled->gdram + gdram_offset, oled->dirty_buf + gdram_offset, size);

  // copy into a buffer with a data control byte in front so it can be sent
  buf = rp2040_oled_alloc_data_buf(size);
  memcpy(buf, oled->gdram + gdram_offset, size);

  // set_position takes a pixel row, so turn the page back into one
  if (!rp2040_oled_set_position(oled, x, y * PAGE_BITS, true)) {
    return false;
  }
  if (rp2040_i2c_write(oled, buf - 1, size + 1) != size + 1) {
    rp2040_oled_free_data_buf(buf);
    return false;
  }

  rp2040_oled_free_data_buf(buf);
  return true;
}

// sends only what changed since the last flush. it goes page by page and
// finds runs of changed columns next to each other, then sends each run in
// one i2c write so it isn't sending one byte at a time
bool rp2040_oled_flush(rp2040_oled_t *oled) {
  if (!oled->is_dirty)
    return true;

  for (uint8_t y = 0; y < oled->height / PAGE_BITS; y++) {
    // start column and length of the current run of changed columns
    uint8_t xstart = 0;
    uint8_t width = 0;

    if (oled->use_doublebuf) {
      // doublebuf: a column changed if the drawn buffer (dirty_buf) and the
      // on screen copy (gdram) don't match
      for (uint8_t x = 0; x < oled->width; x++) {
        if (*(oled->gdram + (y * oled->width + x)) !=
            *(oled->dirty_buf + (y * oled->width + x))) {
          if (width == 0)
            xstart = x;
          width++;
        } else {
          // run just ended, send it
          if (width != 0) {
            size_t gdram_offset = xstart + (y * oled->width);
            rp2040_oled_render_gdram(oled, xstart, y, gdram_offset, width);

            width = 0;
          }
        }
      }
    } else {
      // no doublebuf: dirty_buf is a bitmap, 1 bit per column. each byte
      // covers 8 columns, and bytes that are 0 get skipped entirely
      for (uint8_t xpage = 0; xpage < oled->width / 8; xpage++) {
        uint8_t page = oled->dirty_buf[y * (oled->width / 8) + xpage];
        if (page) {
          for (uint8_t dx = 0; dx < 8; dx++) {
            if (page & 1 << dx) {
              if (width == 0) {
                xstart = xpage * 8 + dx;
              }
              width++;
            } else {
              // run just ended, send it
              if (width != 0) {
                size_t gdram_offset = xstart + (y * oled->width);
                rp2040_oled_render_gdram(oled, xstart, y, gdram_offset, width);

                width = 0;
              }
            }
          }
        }
      }
    }

    // a run that went all the way to the right edge
    if (width != 0) {
      size_t gdram_offset = xstart + (y * oled->width);
      rp2040_oled_render_gdram(oled, xstart, y, gdram_offset, width);

      width = 0;
    }
  }

  oled->is_dirty = false;

  // the bitmap gets cleared after each flush. the doublebuf doesn't need
  // to be, since gdram now matches it
  if (!oled->use_doublebuf) {
    memset(oled->dirty_buf, 0x00, oled->dirty_buf_size);
  }

  oled->cursor.x = 0;
  oled->cursor.y = 0;

  return true;
}

// sends every page of gdram in full, no matter what's marked dirty
bool rp2040_oled_force_flush(rp2040_oled_t *oled) {
  for (uint8_t y = 0; y < oled->height / PAGE_BITS; y++) {
    size_t gdram_offset = y * oled->width;
    rp2040_oled_render_gdram(oled, 0, y, gdram_offset, oled->width);
  }

  oled->is_dirty = false;

  oled->cursor.x = 0;
  oled->cursor.y = 0;

  return true;
}

// core write that all the drawing functions go through. writes `size`
// bytes from buf into the framebuffer at the cursor, combining them with
// what's there based on color (see rp2040_oled_color_t). then it either marks
// those columns dirty for later or, if render is true, sends them now.
// buf must have a spare byte in front of it (buf[-1]) for the control byte,
// which is why callers use alloc_data_buf or pass in buf + 1.
// note: buf gets overwritten with the combined bytes
static bool rp2040_oled_write_gdram(rp2040_oled_t *oled, uint8_t *buf,
                                    size_t size, rp2040_oled_color_t color,
                                    bool render) {
  size_t gdram_offset = oled->cursor.x + (oled->cursor.y * oled->width);
  // drawing goes into the back buffer with doublebuf, otherwise straight
  // into gdram
  uint8_t *gdram = oled->use_doublebuf ? oled->dirty_buf : oled->gdram;

  // won't wrap onto the next page
  if (oled->cursor.x + size > oled->width)
    return false;

  // WHITE ORs (only turns pixels on), BLACK ANDs (only turns pixels off),
  // FULL_BYTE skips this and just overwrites
  if (color != OLED_COLOR_FULL_BYTE) {
    for (size_t i = 0; i < size; i++) {
      if (color == OLED_COLOR_WHITE)
        buf[i] = buf[i] | *(gdram + gdram_offset + i);
      else if (color == OLED_COLOR_BLACK)
        buf[i] = buf[i] & *(gdram + gdram_offset + i);
    }
  }
  memcpy(gdram + gdram_offset, buf, size);

  if (!render) {
    // no doublebuf: set this column's bit in the dirty bitmap.
    // with doublebuf, flush finds changes by diffing so no bitmap is needed
    if (!oled->use_doublebuf)
      for (uint8_t x = oled->cursor.x; x < oled->cursor.x + size; x++)
        oled->dirty_buf[oled->cursor.y * (oled->width / 8) + x / 8] |= 1
                                                                       << x % 8;

    oled->is_dirty = true;
  } else if (oled->use_doublebuf) {
    oled->is_dirty = true;
  }

  oled->cursor.x += size;

  if (!render)
    return true;

  // doublebuf: flush diffs and sends everything that changed.
  // otherwise: send these bytes directly. this relies on the display's
  // write pointer already being at the cursor (set_position with render)
  return oled->use_doublebuf
             ? rp2040_oled_flush(oled)
             : rp2040_i2c_write(oled, buf - 1, size + 1) == size + 1;
}

// sets every byte of the framebuffer to fill_byte, one page row at a time
static bool rp2040_oled_fill(rp2040_oled_t *oled, uint8_t fill_byte,
                             bool render) {
  uint8_t *fill_buf;
  size_t fill_buf_size = oled->width;

  // one full row of fill_byte, reused for every page
  fill_buf = rp2040_oled_alloc_data_buf(fill_buf_size);
  memset(fill_buf, fill_byte, fill_buf_size);

  for (uint8_t y = 0; y < oled->height; y += PAGE_BITS) {
    if (!rp2040_oled_set_position(oled, 0, y, false)) {
      rp2040_oled_free_data_buf(fill_buf);
      return false;
    }

    if (!rp2040_oled_write_gdram(oled, fill_buf, fill_buf_size,
                                 OLED_COLOR_FULL_BYTE, false)) {
      rp2040_oled_free_data_buf(fill_buf);
      return false;
    }
  }

  if (render)
    rp2040_oled_flush(oled);

  rp2040_oled_free_data_buf(fill_buf);
  return true;
}

// builds the columns for the whole string in one buffer and writes it in
// one go. each character is 6 columns: a blank spacer column then the 5
// font columns. drawn with OR so it doesn't erase what's behind the text.
// note: characters below ' ' (32) aren't checked for and read outside the
// font table
bool rp2040_oled_write_string(rp2040_oled_t *oled, uint8_t x, uint8_t y,
                              char *msg, size_t len, bool render) {
  uint8_t *buf;
  size_t buf_size = len * 6;
  size_t i = 0;
  if (x >= oled->width || y >= oled->height)
    return false;

  if (!rp2040_oled_set_position(oled, x, y, render))
    return false;

  buf = rp2040_oled_alloc_data_buf(buf_size);

  for (i = 0; i < len; i++) {
    // font starts at ascii 32 (space)
    uint8_t font_index = msg[i] - 32;

    buf[i * 6] = 0x00;
    memcpy(buf + (1 + (i * 6)), font_6x8 + (font_index * 5), 5);
  }

  if (!rp2040_oled_write_gdram(oled, buf, buf_size, OLED_COLOR_WHITE, render)) {
    rp2040_oled_free_data_buf(buf);
    return false;
  }

  rp2040_oled_free_data_buf(buf);
  return true;
}

// a pixel is one bit inside a byte, so this reads the byte that holds it,
// sets or clears just that bit, and writes the byte back
bool rp2040_oled_set_pixel(rp2040_oled_t *oled, uint8_t x, uint8_t y,
                           rp2040_oled_color_t color, bool render) {
  bool ret = true;
  uint8_t page = y / PAGE_BITS;
  uint8_t page_offset = y % PAGE_BITS;
  uint8_t bit_mask = 1 << page_offset;
  // buf[0] is the spare control byte that write_gdram expects at buf[-1]
  uint8_t buf[2] = {OLED_CB_DATA_BIT, 0x00};
  uint8_t *gdram = oled->use_doublebuf ? oled->dirty_buf : oled->gdram;

  if (x >= oled->width || y >= oled->height)
    return false;

  buf[1] = gdram[x + (page * oled->width)];
  if (color == OLED_COLOR_WHITE)
    buf[1] |= bit_mask;
  else if (color == OLED_COLOR_BLACK)
    buf[1] &= ~bit_mask;
  else
    return false;

  if (!rp2040_oled_set_position(oled, x, page * PAGE_BITS, render))
    return false;

  if (!rp2040_oled_write_gdram(oled, buf + 1, 1, color, render))
    ret = false;

  return ret;
}

// vertical lines get a fast path that writes whole bytes (up to 8 pixels at
// once) instead of going pixel by pixel. every other line uses bresenham's
// line algorithm with set_pixel
bool rp2040_oled_draw_line(rp2040_oled_t *oled, uint8_t x0, uint8_t y0,
                           uint8_t x1, uint8_t y1, rp2040_oled_color_t color,
                           bool render) {
  // bresenham setup: distance and step direction on each axis
  int16_t dx = abs(x1 - x0);
  int16_t dy = -abs(y1 - y0);
  int8_t sx = x0 < x1 ? 1 : -1;
  int8_t sy = y0 < y1 ? 1 : -1;
  int16_t err = dx + dy;
  int16_t err2;
  // white starts all off and sets bits, black starts all on and clears
  // bits, so the mask works with write_gdram's OR / AND
  uint8_t bit_mask = (color == OLED_COLOR_WHITE) ? 0x00 : 0xff;
  uint8_t buf[2] = {OLED_CB_DATA_BIT, 0x00};

  if (x0 >= oled->width || x1 >= oled->width || y0 >= oled->height ||
      y1 >= oled->height)
    return false;

  if (x0 == x1) {
    // vertical line that stays inside one page: a single byte
    if (y0 / PAGE_BITS == y1 / PAGE_BITS) {
      for (uint8_t y = y0; y != y1; y += sy) {
        uint8_t bitshift =
            1 << (sy == 1 ? y % PAGE_BITS : PAGE_BITS - y % PAGE_BITS);
        if (color == OLED_COLOR_WHITE)
          bit_mask |= bitshift;
        else
          bit_mask &= ~bitshift;
      }
      rp2040_oled_set_position(oled, x0, y0, render);
      buf[1] = bit_mask;
      rp2040_oled_write_gdram(oled, buf + 1, 1, color, render);
    } else {
      // vertical line across several pages: a partial byte for the top
      // page, full bytes for the pages in the middle, and a partial byte
      // for the bottom page
      uint8_t tshift, bshift, y;
      // always draw top to bottom
      if (y0 > y1) {
        y = y1;
        y1 = y0;
        y0 = y;
      }

      // tshift = pixels from y0 to the bottom of its page
      // bshift = pixels from the top of y1's page down to y1
      tshift = PAGE_BITS - y0 % PAGE_BITS;
      bshift = y1 % PAGE_BITS;
      y = y0;

      // top page: bits y0 % 8 through 7, so from y0 down to the bottom
      // of that page
      if (tshift) {
        bit_mask = (color == OLED_COLOR_WHITE) ? 0x00 : 0xff;
        for (uint8_t i = 0; i < tshift; i++) {
          uint8_t bitshift = 1 << ((PAGE_BITS - 1) - i);
          if (color == OLED_COLOR_WHITE)
            bit_mask |= bitshift;
          else
            bit_mask &= ~bitshift;
        }
        rp2040_oled_set_position(oled, x0, y, false);
        buf[1] = bit_mask;
        rp2040_oled_write_gdram(oled, buf + 1, 1, color, false);
        y += tshift;
      }

      // middle pages: every pixel in the byte
      buf[1] = (color == OLED_COLOR_WHITE) ? 0xff : 0x00;
      while (y / PAGE_BITS < y1 / PAGE_BITS) {
        rp2040_oled_set_position(oled, x0, y, false);
        rp2040_oled_write_gdram(oled, buf + 1, 1, color, false);
        y += PAGE_BITS;
      }

      // bottom page: bits 0 through bshift - 1, so from the top of the
      // page down to (but not including) y1
      if (bshift) {
        bit_mask = (color == OLED_COLOR_WHITE) ? 0x00 : 0xff;
        for (uint8_t i = 0; i < bshift; i++) {
          uint8_t bitshift = 1 << i;
          if (color == OLED_COLOR_WHITE)
            bit_mask |= bitshift;
          else
            bit_mask &= ~bitshift;
        }
        rp2040_oled_set_position(oled, x0, y, false);
        buf[1] = bit_mask;
        rp2040_oled_write_gdram(oled, buf + 1, 1, color, false);
        y += bshift;
      }

      if (render)
        rp2040_oled_flush(oled);
    }
    return true;
  }

  // bresenham: step along the line one pixel at a time. err tracks how far
  // the drawn pixels are from the real line, and decides whether the next
  // step moves in x, y, or both
  while (1) {
    rp2040_oled_set_pixel(oled, x0, y0, color, false);
    if (x0 == x1 && y0 == y1)
      break;

    err2 = 2 * err;
    if (err2 >= dy) {
      if (x0 == x1)
        break;
      err = err + dy;
      x0 = x0 + sx;
    }

    if (err2 <= dx) {
      if (y0 == y1)
        break;
      err = err + dx;
      y0 = y0 + sy;
    }
  }

  if (render)
    rp2040_oled_flush(oled);
  return true;
}

// outline = 4 lines. filled = one vertical line per column, since vertical
// lines take the fast byte-at-a-time path in draw_line
bool rp2040_oled_draw_rectangle(rp2040_oled_t *oled, uint8_t x0, uint8_t y0,
                                uint8_t x1, uint8_t y1,
                                rp2040_oled_color_t color, bool fill,
                                bool render) {
  uint8_t tmp = 0;

  // swap corners so (x0, y0) is the top left one
  if (x0 > x1) {
    tmp = x0;
    x0 = x1;
    x1 = tmp;
  }

  if (y0 > y1) {
    tmp = y0;
    y0 = y1;
    y1 = tmp;
  }

  if (!fill) {
    rp2040_oled_draw_line(oled, x0, y0, x0, y1, color, false);
    rp2040_oled_draw_line(oled, x0, y0, x1, y0, color, false);
    rp2040_oled_draw_line(oled, x1, y0, x1, y1, color, false);
    rp2040_oled_draw_line(oled, x0, y1, x1, y1, color, false);
  } else {
    for (uint8_t x = x0; x <= x1; x++) {
      rp2040_oled_draw_line(oled, x, y0, x, y1, color, false);
    }
  }

  if (render)
    rp2040_oled_flush(oled);

  return true;
}

// zero the framebuffer and send it to the screen
bool rp2040_oled_clear(rp2040_oled_t *oled) {
  return rp2040_oled_fill(oled, 0x00, true);
}

// zero the framebuffer only, the screen changes on the next flush
bool rp2040_oled_clear_gdram(rp2040_oled_t *oled) {
  return rp2040_oled_fill(oled, 0x00, false);
}

// draws a sprite that's already in the page format (width bytes per page,
// bit 0 on top). the work here is clipping and lining things up:
// - parts hanging off the left/top get skipped (xshift / yshift)
// - parts hanging off the right/bottom get cut (width / height shrink)
// - if the sprite doesn't start on a page boundary its bits are shifted
//   into a temporary copy (osprite) so they line up with the screen's pages
// then each page row of the sprite is written with write_gdram
bool rp2040_oled_draw_sprite(rp2040_oled_t *oled, const uint8_t *sprite,
                             int16_t x, int16_t y, uint8_t width,
                             uint8_t height, rp2040_oled_color_t color,
                             bool render) {
  bool ret = true;
  uint8_t *buf;
  size_t buf_size;
  // the sprite's real width, needed to step between its pages even after
  // width gets clipped
  uint8_t orig_width = width;
  const uint8_t *final_sprite = sprite;
  uint8_t orig_pages, final_pages;
  uint8_t *osprite = NULL;
  size_t osprite_size;
  // how many columns / rows got clipped off the left / top
  uint8_t xshift = 0, yshift = 0;

  // completely off screen to the left or top
  if (x + width < 0 || y + height < 0)
    return false;

  // clip left and top
  if (x < 0) {
    width += x;
    xshift = -x;
    x = 0;
  }

  if (y < 0) {
    height += y;
    yshift = -y;
    y = 0;
  }

  // clip right and bottom
  if (x + width > oled->width)
    width = oled->width - x;

  if (y + height > oled->height)
    height = oled->height - y;

  // pages needed, rounded up
  orig_pages = (height + PAGE_BITS - 1) / PAGE_BITS;
  final_pages = orig_pages;

  // not page aligned (or clipped at the top): build a shifted copy
  if ((y % PAGE_BITS != 0) || yshift != 0) {
    // yoffset = how many bits to shift by, pshift = whole sprite pages
    // clipped off the top
    uint8_t yoffset = (y + yshift) % PAGE_BITS;
    uint8_t pshift = yshift / PAGE_BITS;
    // shifting down can spill into one more page than the sprite had
    final_pages = (height + yoffset + PAGE_BITS - 1) / PAGE_BITS;
    osprite_size = orig_width * final_pages;
    osprite = malloc(osprite_size);
    memset(osprite, 0x00, osprite_size);
    final_sprite = osprite;
    for (uint8_t cur_page = 0; cur_page < final_pages; cur_page++) {
      for (uint8_t cur_x = 0; cur_x < orig_width; cur_x++) {
        if (y != 0) {
          // sprite starts partway down a page, so shift it down.
          // each output byte = the bottom bits of the sprite page above it
          // + the top bits of the matching sprite page
          if ((cur_page) > 0)
            osprite[cur_x + cur_page * orig_width] |=
                sprite[cur_x + (cur_page - 1) * orig_width] >>
                (PAGE_BITS - yoffset);

          if (cur_page < orig_pages)
            osprite[cur_x + cur_page * orig_width] |=
                sprite[cur_x + cur_page * orig_width] << yoffset;
        } else {
          // y was negative and got clamped to 0, so shift up instead:
          // skip the clipped pages and pull bits up from the next page
          osprite[cur_x + cur_page * orig_width] |=
              sprite[cur_x + (pshift + cur_page) * orig_width] >> yoffset;

          if (cur_page < orig_pages)
            osprite[cur_x + cur_page * orig_width] |=
                sprite[cur_x + (pshift + cur_page + 1) * orig_width]
                << (PAGE_BITS - yoffset);
        }
      }
    }
  }

  // one page row of the (clipped) sprite at a time
  buf_size = width;
  buf = rp2040_oled_alloc_data_buf(buf_size);

  for (uint8_t cur_page = 0; cur_page < final_pages; cur_page++) {
    if (!rp2040_oled_set_position(oled, x, y + cur_page * PAGE_BITS, false)) {
      ret = false;
      goto out;
    }
    // skip the columns clipped off the left
    memcpy(buf, final_sprite + xshift + (cur_page * orig_width), width);
    // memcpy(buf, final_sprite + xshift + ((yshift + cur_page) * orig_width),
    // width);
    // last page and the sprite ends partway through it: mask off the rows
    // below the sprite's bottom edge
    if (cur_page == final_pages - 1 && (y + height) % PAGE_BITS) {
      uint8_t mask = 0x00;
      for (uint8_t i = 0; i < (y + height) % PAGE_BITS; i++) {
        mask |= 1 << i;
      }

      for (uint8_t i = 0; i < width; i++) {
        *(buf + i) &= mask;
      }
    }
    if (!rp2040_oled_write_gdram(oled, buf, buf_size, color, false)) {
      ret = false;
      goto out;
    }
  }

  if (render)
    rp2040_oled_flush(oled);

  // cleanup for both the success and error paths
out:
  rp2040_oled_free_data_buf(buf);
  if (osprite)
    free(osprite);
  return ret;
}

// turns a row by row bitmap (1 bit per pixel, msb first, `pitch` bytes per
// row) into the page format draw_sprite wants, then draws it with that
bool rp2040_oled_draw_sprite_pitched(rp2040_oled_t *oled, uint8_t *sprite,
                                     int16_t x, int16_t y, uint8_t width,
                                     uint8_t height, uint8_t pitch,
                                     rp2040_oled_color_t color, bool render) {
  bool ret = true;
  uint8_t *mem_sprite = NULL;
  size_t mem_sprite_size = width * ((height + PAGE_BITS - 1) / PAGE_BITS);

  mem_sprite = malloc(mem_sprite_size);
  memset(mem_sprite, 0x00, mem_sprite_size);

  for (uint8_t cy = 0; cy < height; cy++) {
    // src walks across this row of the input, dst is the page row this
    // pixel row ends up in, and dst_mask is its bit inside each byte
    uint8_t cpage = cy / PAGE_BITS;
    uint8_t sx = 0;
    uint8_t *src = sprite + (cy * pitch);
    uint8_t *dst = mem_sprite + (cpage * width);
    uint8_t src_mask = 0x80;
    uint8_t dst_mask = 1 << (cy & (PAGE_BITS - 1));
    for (uint8_t dx = 0; dx < width; dx++) {
      if (*(src + sx) & src_mask)
        *(dst + dx) |= dst_mask;
      // next input bit, moving to the next input byte after 8
      src_mask >>= 1;
      if (!src_mask) {
        src_mask = 0x80;
        sx++;
      }
    }
  }

  ret = rp2040_oled_draw_sprite(oled, mem_sprite, x, y, width, height, color,
                                render);
  free(mem_sprite);
  return ret;
}

// midpoint circle algorithm. it walks one eighth of the circle (dx, dy) and
// mirrors each point into all 8 octants. filled circles draw vertical lines
// between the mirrored points instead of single pixels
bool rp2040_oled_draw_circle(rp2040_oled_t *oled, int16_t x, int16_t y,
                             uint8_t r, rp2040_oled_color_t color, bool fill,
                             bool render) {
  // start at the rightmost point (r, 0) and go until the 45 degree mark
  // where dx and dy meet
  uint8_t dx = r, dy = 0;
  int16_t t1 = r / 16;
  int16_t t2;

  while (dx >= dy) {
    if (fill) {
      rp2040_oled_draw_line(oled, x + dx, y + dy, x + dx, y - dy, color, false);
      rp2040_oled_draw_line(oled, x - dx, y + dy, x - dx, y - dy, color, false);
      rp2040_oled_draw_line(oled, x + dy, y + dx, x + dy, y - dx, color, false);
      rp2040_oled_draw_line(oled, x - dy, y + dx, x - dy, y - dx, color, false);
    } else {
      rp2040_oled_set_pixel(oled, x + dx, y + dy, color, false);
      rp2040_oled_set_pixel(oled, x + dx, y - dy, color, false);
      rp2040_oled_set_pixel(oled, x - dx, y + dy, color, false);
      rp2040_oled_set_pixel(oled, x - dx, y - dy, color, false);
      rp2040_oled_set_pixel(oled, x + dy, y + dx, color, false);
      rp2040_oled_set_pixel(oled, x + dy, y - dx, color, false);
      rp2040_oled_set_pixel(oled, x - dy, y + dx, color, false);
      rp2040_oled_set_pixel(oled, x - dy, y - dx, color, false);
    }

    // always step up one row. t1 builds up the error, and once it goes
    // past dx it's time to step one column in too
    dy++;
    t1 += dy;
    t2 = t1 - dx;
    if (t2 >= 0) {
      t1 = t2;
      dx--;
    }
  }

  if (render)
    rp2040_oled_flush(oled);

  return true;
}

// midpoint ellipse algorithm. it walks one quarter of the ellipse starting
// at the top (0, ry) and mirrors each point into the other 3 quarters.
// it's split into two regions: region 1 is the flatter part near the top
// where x changes faster than y, region 2 is the steeper part near the side
// where y changes faster. d1 / d2 decide whether the next point steps in
// one axis or both
bool rp2040_oled_draw_ellipse(rp2040_oled_t *oled, int16_t x, int16_t y,
                              uint8_t rx, uint8_t ry, rp2040_oled_color_t color,
                              bool fill, bool render) {
  float dx, dy, d1, d2;
  int16_t sx, sy;
  int32_t rx2, ry2;

  // equal radii is just a circle, and the circle code is cheaper
  if (rx == ry)
    return rp2040_oled_draw_circle(oled, x, y, rx, color, fill, render);

  sx = 0;
  sy = ry;

  rx2 = rx * rx;
  ry2 = ry * ry;

  // region 1 starting decision value, and the slope terms
  d1 = ry2 - (rx2 * ry) + (0.25 * rx2);
  dx = 2 * ry2 * sx;
  dy = 2 * rx2 * sy;

  // region 1: until the slope gets steeper than -1
  while (dx < dy) {
    if (fill) {
      rp2040_oled_draw_line(oled, x + sx, y - sy, x + sx, y + sy, color, false);
      rp2040_oled_draw_line(oled, x - sx, y - sy, x - sx, y + sy, color, false);
    } else {
      rp2040_oled_set_pixel(oled, x + sx, y - sy, color, false);
      rp2040_oled_set_pixel(oled, x + sx, y + sy, color, false);
      rp2040_oled_set_pixel(oled, x - sx, y - sy, color, false);
      rp2040_oled_set_pixel(oled, x - sx, y + sy, color, false);
    }

    // inside the ellipse: only step x. otherwise step x and y
    if (d1 < 0) {
      sx++;
      dx = dx + (2 * ry2);
      d1 = d1 + dx + ry2;
    } else {
      sx++;
      sy--;
      dx = dx + (2 * ry2);
      dy = dy - (2 * rx2);
      d1 = d1 + dx - dy + ry2;
    }
  }

  // region 2: starts where region 1 stopped and goes down to y = 0
  d2 = (ry2 * ((sx + 0.5) * (sx + 0.5))) + (rx2 * ((sy - 1) * (sy - 1))) -
       (rx2 * ry2);
  while (sy >= 0) {
    if (fill) {
      rp2040_oled_draw_line(oled, x + sx, y - sy, x + sx, y + sy, color, false);
      rp2040_oled_draw_line(oled, x - sx, y - sy, x - sx, y + sy, color, false);
    } else {
      rp2040_oled_set_pixel(oled, x + sx, y - sy, color, false);
      rp2040_oled_set_pixel(oled, x + sx, y + sy, color, false);
      rp2040_oled_set_pixel(oled, x - sx, y - sy, color, false);
      rp2040_oled_set_pixel(oled, x - sx, y + sy, color, false);
    }

    // outside the ellipse: only step y. otherwise step y and x
    if (d2 > 0) {
      sy--;
      dy = dy - (2 * rx2);
      d2 = d2 + rx2 - dy;
    } else {
      sy--;
      sx++;
      dx = dx + (2 * ry2);
      dy = dy - (2 * rx2);
      d2 = d2 + dx - dy + rx2;
    }
  }

  if (render)
    rp2040_oled_flush(oled);

  return true;
}
