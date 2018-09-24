#include "guest/pvr/pvr.h"
#include "core/time.h"
#include "guest/dreamcast.h"
#include "guest/holly/holly.h"
#include "guest/memory.h"
#include "guest/pvr/ta.h"
#include "guest/scheduler.h"
#include "guest/sh4/sh4.h"
#include "stats.h"

static struct reg_cb pvr_cb[PVR_NUM_REGS];

/* the dreamcast has 8MB of vram, split into two 4MB banks, with two ways of
   accessing it:

   * 64-bit access path - each 4MB bank is interleaved every 32-bits, enabling
     a 64-bit data bus to be populated from both banks in parallel
   * 32-bit access path - each 4MB bank is accessed sequentially one after the
     other

   by default (when SB_LMMODE0/1=0) the ta will use the 64-bit access path for
   poly and texture transfers. due to this being the default for the ta, our
   internal vram layout matches the 64-bit access paths view, meaning 32-bit
   accesses will have to be converted to an interleaved address */
static uint32_t VRAM64(uint32_t addr32) {
  const uint32_t bank_size = 0x00400000;
  uint32_t bank = addr32 & bank_size;
  uint32_t offset = addr32 & (bank_size - 1);
  return ((offset & ~0x3) << 1) | (bank >> 20) | (offset & 0x3);
}

/* on the real hardware, the CORE copies it's final accumulation buffer to a
   framebuffer in texture memory, where the DVE then reads it from to produce
   the actual video output

   when emulating, this process is skipped, and the output is instead rendered
   directly to the host's default framebuffer. this avoids several unnecessary
   copies between the gpu and cpu, and is significantly faster

   the downside to this approach being that it doesn't work for programs such
   as the IP.BIN license screen code, which write directly to the framebuffer,
   as that memory is never read from to produce video output

   to support these direct writes to the framebuffer, the PVR code marks each
   framebuffer during a STARTRENDER request by writing a cookie to its memory,
   and then checks for this cookie during the vblank. if the cookie doesn't
   exist, it's assumed that the framebuffer memory is dirty and the texture
   memory is copied and passed to the client to render */
#define PVR_FB_COOKIE 0xdeadbeef

static void pvr_framebuffer_size(struct pvr *pvr, int *width, int *height) {
  *width = pvr->FB_R_SIZE->x + 1;
  *height = pvr->FB_R_SIZE->y + 1;

  /* FB_R_SIZE specifies x in 32-bit units, scale as necessary if framebuffer
     depth is less than 32-bit */
  switch (pvr->FB_R_CTRL->fb_depth) {
    case 0:
    case 1:
      *width *= 2;
      break;
    case 2:
      *width *= 4;
      *width /= 3;
      break;
  }

  /* if interlacing, full framebuffer height is double */
  if (pvr->SPG_CONTROL->interlace) {
    *height *= 2;
  }
}

static int pvr_test_framebuffer(struct pvr *pvr, uint32_t addr) {
  uint32_t data = *(uint32_t *)&pvr->vram[VRAM64(addr)];
  return data != PVR_FB_COOKIE;
}

static void pvr_mark_framebuffer(struct pvr *pvr, uint32_t addr) {
  /* don't mark framebuffers which are being  used as textures */
  if (addr & 0x01000000) {
    return;
  }

  *(uint32_t *)&pvr->vram[VRAM64(addr)] = PVR_FB_COOKIE;

  /* it's not enough to just mark the starting address of this framebuffer. next
     frame, this framebuffer could be used as field 2, in which case FB_R_SOF2
     would be set to addr + line_size + line_mod */
  const uint32_t line_width[] = {320, 640};
  const uint32_t line_bpp[] = {2, 3, 4};
  const uint32_t line_scale[] = {1, 2};

  for (int i = 0; i < ARRAY_SIZE(line_width); i++) {
    for (int j = 0; j < ARRAY_SIZE(line_bpp); j++) {
      for (int k = 0; k < ARRAY_SIZE(line_scale); k++) {
        uint32_t next_line = addr + line_width[i] * line_bpp[j] * line_scale[k];
        *(uint32_t *)&pvr->vram[VRAM64(next_line)] = PVR_FB_COOKIE;
      }
    }
  }
}

static int pvr_update_framebuffer(struct pvr *pvr) {
  uint32_t fields[2] = {*pvr->FB_R_SOF1, *pvr->FB_R_SOF2};
  int num_fields = pvr->SPG_CONTROL->interlace ? 2 : 1;
  int field = pvr->SPG_STATUS->fieldnum;

  if (!pvr->FB_R_CTRL->fb_enable) {
    return 0;
  }

  /* don't do anything if the framebuffer hasn't been written to */
  if (!pvr_test_framebuffer(pvr, fields[field])) {
    return 0;
  }

  pvr_framebuffer_size(pvr, &pvr->framebuffer_w, &pvr->framebuffer_h);

  /* convert framebuffer into a 24-bit RGB pixel buffer */
  uint8_t *dst = pvr->framebuffer;
  uint8_t *src = pvr->vram;

  /* values in FB_R_SIZE are in 32-bit units */
  int line_mod = (pvr->FB_R_SIZE->mod << 2) - 4;
  int x_size = (pvr->FB_R_SIZE->x + 1) << 2;
  int y_size = (pvr->FB_R_SIZE->y + 1);

  /* TODO use fb_concat */

  switch (pvr->FB_R_CTRL->fb_depth) {
    case 0: {
      for (int y = 0; y < y_size; y++) {
        for (int n = 0; n < num_fields; n++) {
          for (int x = 0; x < x_size; x += 2) {
            uint16_t rgb = *(uint16_t *)&src[VRAM64(fields[n])];
            dst[0] = (rgb & 0b0111110000000000) >> 7;
            dst[1] = (rgb & 0b0000001111100000) >> 2;
            dst[2] = (rgb & 0b0000000000011111) << 3;
            fields[n] += 2;
            dst += 3;
          }
          fields[n] += line_mod;
        }
      }
    } break;

    case 1: {
      for (int y = 0; y < y_size; y++) {
        for (int n = 0; n < num_fields; n++) {
          for (int x = 0; x < x_size; x += 2) {
            uint16_t rgb = *(uint16_t *)&src[VRAM64(fields[n])];
            dst[0] = (rgb & 0b1111100000000000) >> 8;
            dst[1] = (rgb & 0b0000011111100000) >> 3;
            dst[2] = (rgb & 0b0000000000011111) << 3;
            fields[n] += 2;
            dst += 3;
          }
          fields[n] += line_mod;
        }
      }
    } break;
    case 2: {
      for (int y = 0; y < y_size; y++) {
        for (int n = 0; n < num_fields; n++) {
          for (int x = 0; x < x_size; x += 3) {
            uint8_t *rgb = &src[VRAM64(fields[n])];
            dst[0] = rgb[2];
            dst[1] = rgb[1];
            dst[2] = rgb[0];
            fields[n] += 3;
            dst += 3;
          }
          fields[n] += line_mod;
        }
      }
    } break;
    case 3: {
      for (int y = 0; y < y_size; y++) {
        for (int n = 0; n < num_fields; n++) {
          for (int x = 0; x < x_size; x += 4) {
            uint8_t *krgb = &src[VRAM64(fields[n])];
            dst[0] = krgb[2];
            dst[1] = krgb[1];
            dst[2] = krgb[0];
            fields[n] += 4;
            dst += 3;
          }
          fields[n] += line_mod;
        }
      }
    } break;
    default:
      LOG_FATAL("pvr_push_framebuffer unexpected fb_depth %d",
                pvr->FB_R_CTRL->fb_depth);
      break;
  }

  dc_push_pixels(pvr->dc, pvr->framebuffer, pvr->framebuffer_w,
                 pvr->framebuffer_h);

  return 1;
}

static void pvr_vblank_out(struct pvr *pvr) {
  dc_vblank_out(pvr->dc);
}

static void pvr_vblank_in(struct pvr *pvr) {
  prof_counter_add(COUNTER_pvr_vblanks, 1);

  /* if STARTRENDER wasn't written to this frame, check to see if the
     framebuffer was written to directly */
  if (!pvr->got_startrender) {
    pvr_update_framebuffer(pvr);
  } else {
    pvr->got_startrender = 0;
  }

  /* flip field */
  if (pvr->SPG_CONTROL->interlace) {
    pvr->SPG_STATUS->fieldnum = !pvr->SPG_STATUS->fieldnum;
  } else {
    pvr->SPG_STATUS->fieldnum = 0;
  }

  dc_vblank_in(pvr->dc, pvr->VO_CONTROL->blank_video);
}

static void pvr_next_scanline(void *data) {
  struct pvr *pvr = data;
  struct holly *hl = pvr->dc->holly;
  struct scheduler *sched = pvr->dc->sched;

  uint32_t num_lines = pvr->SPG_LOAD->vcount + 1;
  pvr->current_line = (pvr->current_line + 1) % num_lines;

  /* hblank in */
  switch (pvr->SPG_HBLANK_INT->hblank_int_mode) {
    case 0x0:
      if (pvr->current_line == pvr->SPG_HBLANK_INT->line_comp_val) {
        holly_raise_interrupt(hl, HOLLY_INT_PCHIINT);
      }
      break;
    case 0x2:
      holly_raise_interrupt(hl, HOLLY_INT_PCHIINT);
      break;
    default:
      LOG_FATAL("unsupported hblank interrupt mode");
      break;
  }

  /* vblank in */
  if (pvr->current_line == pvr->SPG_VBLANK_INT->vblank_in_line_number) {
    holly_raise_interrupt(hl, HOLLY_INT_PCVIINT);
  }

  /* vblank out */
  if (pvr->current_line == pvr->SPG_VBLANK_INT->vblank_out_line_number) {
    holly_raise_interrupt(hl, HOLLY_INT_PCVOINT);
  }

  int was_vsync = pvr->SPG_STATUS->vsync;
  if (pvr->SPG_VBLANK->vbstart < pvr->SPG_VBLANK->vbend) {
    pvr->SPG_STATUS->vsync = pvr->current_line >= pvr->SPG_VBLANK->vbstart &&
                             pvr->current_line < pvr->SPG_VBLANK->vbend;
  } else {
    pvr->SPG_STATUS->vsync = pvr->current_line >= pvr->SPG_VBLANK->vbstart ||
                             pvr->current_line < pvr->SPG_VBLANK->vbend;
  }
  pvr->SPG_STATUS->scanline = pvr->current_line;

  if (!was_vsync && pvr->SPG_STATUS->vsync) {
    pvr_vblank_in(pvr);
  } else if (was_vsync && !pvr->SPG_STATUS->vsync) {
    pvr_vblank_out(pvr);
  }

  /* reschedule */
  pvr->line_timer = sched_start_timer(sched, &pvr_next_scanline, pvr,
                                      HZ_TO_NANO(pvr->line_clock));
}

static void pvr_reconfigure_spg(struct pvr *pvr) {
  struct scheduler *sched = pvr->dc->sched;

  /* scale pixel clock frequency */
  int pixel_clock = 13500000;
  if (pvr->FB_R_CTRL->vclk_div) {
    pixel_clock *= 2;
  }

  /* hcount is number of pixel clock cycles per line - 1 */
  pvr->line_clock = pixel_clock / (pvr->SPG_LOAD->hcount + 1);
  if (pvr->SPG_CONTROL->interlace) {
    pvr->line_clock *= 2;
  }

  const char *mode = "vga";
  if (pvr->SPG_CONTROL->NTSC == 1) {
    mode = "ntsc";
  } else if (pvr->SPG_CONTROL->PAL == 1) {
    mode = "pal";
  }

  LOG_INFO(
      "pvr_reconfigure_spg mode=%s interlace=%d pixel_clock=%d line_clock=%d "
      "hcount=%d hbstart=%d hbend=%d vcount=%d vbstart=%d vbend=%d",
      mode, pvr->SPG_CONTROL->interlace, pixel_clock, pvr->line_clock,
      pvr->SPG_LOAD->hcount, pvr->SPG_HBLANK->hbstart, pvr->SPG_HBLANK->hbend,
      pvr->SPG_LOAD->vcount, pvr->SPG_VBLANK->vbstart, pvr->SPG_VBLANK->vbend);

  if (pvr->line_timer) {
    sched_cancel_timer(sched, pvr->line_timer);
    pvr->line_timer = NULL;
  }

  pvr->line_timer = sched_start_timer(sched, &pvr_next_scanline, pvr,
                                      HZ_TO_NANO(pvr->line_clock));
}

static int pvr_init(struct device *dev) {
  struct pvr *pvr = (struct pvr *)dev;
  struct dreamcast *dc = pvr->dc;

/* init registers */
#define PVR_REG(offset, name, default, type) \
  pvr->reg[name] = default;                  \
  pvr->name = (type *)&pvr->reg[name];
#include "guest/pvr/pvr_regs.inc"
#undef PVR_REG

  pvr->vram = mem_vram(dc->mem, 0x0);

  /* configure initial vsync interval */
  pvr_reconfigure_spg(pvr);

  return 1;
}

void pvr_vram32_write(struct pvr *pvr, uint32_t addr, uint32_t data,
                      uint32_t mask) {
  addr = VRAM64(addr);
  WRITE_DATA(&pvr->vram[addr]);
}

uint32_t pvr_vram32_read(struct pvr *pvr, uint32_t addr, uint32_t mask) {
  addr = VRAM64(addr);
  return READ_DATA(&pvr->vram[addr]);
}

void pvr_vram64_write(struct pvr *pvr, uint32_t addr, uint32_t data,
                      uint32_t mask) {
  WRITE_DATA(&pvr->vram[addr]);
}

uint32_t pvr_vram64_read(struct pvr *pvr, uint32_t addr, uint32_t mask) {
  /* note, the video ram can't be directly accessed through fastmem, or texture
     cache invalidations will break. this is because textures cache entries
     only watch the physical video ram address, not all of its mirrors */
  return READ_DATA(&pvr->vram[addr]);
}

void pvr_reg_write(struct pvr *pvr, uint32_t addr, uint32_t data,
                   uint32_t mask) {
  uint32_t offset = addr >> 2;
  reg_write_cb write = pvr_cb[offset].write;

  /* ID register is read-only, and the bios will fail to boot if a write
     goes through to this register */
  if (offset == ID) {
    return;
  }

  if (write) {
    write(pvr->dc, data);
    return;
  }

  pvr->reg[offset] = data;
}

uint32_t pvr_reg_read(struct pvr *pvr, uint32_t addr, uint32_t mask) {
  uint32_t offset = addr >> 2;
  reg_read_cb read = pvr_cb[offset].read;

  if (read) {
    return read(pvr->dc);
  }

  return pvr->reg[offset];
}

void pvr_video_size(struct pvr *pvr, int *width, int *height) {
  /* calculate the original internal resolution used by the game based on the
     framebuffer size. this is used to scale the screen space x,y coordinates
     passed to the ta when rendering */
  pvr_framebuffer_size(pvr, width, height);

  /* scale_x signals to scale down the accumulation buffer by half when copying
     to the framebuffer (providing horizontal fsaa), meaning the original video
     width is double the framebufffer width */
  if (pvr->SCALER_CTL->scale_x) {
    *width *= 2;
  }

  /* scale_y is a fixed-point scaler, with 6-bits in the integer and 10-bits in
     the decimal. the accumulation buffer is scaled by 1/scale_y, for example:
     0x200: 2.0x scale
     0x400: 1.0x scale
     0x800: 0.5x scale
     reversing this operation should give us the original video height */
  *height = (*height * pvr->SCALER_CTL->scale_y) >> 10;

  /* if flicker-free type B interlacing is being used, scale the height back
     down, nullifying the effect of scale_y */
  if (pvr->SCALER_CTL->interlace) {
    *height /= 2;
  }
}

void pvr_destroy(struct pvr *pvr) {
  dc_destroy_device((struct device *)pvr);
}

struct pvr *pvr_create(struct dreamcast *dc) {
  struct pvr *pvr =
      dc_create_device(dc, sizeof(struct pvr), "pvr", &pvr_init, NULL);

  return pvr;
}

REG_W32(pvr_cb, SOFTRESET) {
  struct pvr *pvr = dc->pvr;
  struct ta *ta = dc->ta;

  if (!(value & 0x1)) {
    return;
  }

  ta_soft_reset(ta);
}

REG_W32(pvr_cb, STARTRENDER) {
  struct pvr *pvr = dc->pvr;
  struct ta *ta = dc->ta;

  if (!value) {
    return;
  }

  ta_start_render(ta);

  pvr_mark_framebuffer(pvr, *pvr->FB_W_SOF1);
  pvr_mark_framebuffer(pvr, *pvr->FB_W_SOF2);
  pvr->got_startrender = 1;
}

REG_W32(pvr_cb, TA_LIST_INIT) {
  struct pvr *pvr = dc->pvr;
  struct ta *ta = dc->ta;

  if (!(value & 0x80000000)) {
    return;
  }

  ta_list_init(ta);
}

REG_W32(pvr_cb, TA_LIST_CONT) {
  struct pvr *pvr = dc->pvr;
  struct ta *ta = dc->ta;

  if (!(value & 0x80000000)) {
    return;
  }

  ta_list_cont(ta);
}

REG_W32(pvr_cb, TA_YUV_TEX_BASE) {
  struct pvr *pvr = dc->pvr;
  struct ta *ta = dc->ta;

  pvr->TA_YUV_TEX_BASE->full = value;

  ta_yuv_init(ta);
}

REG_W32(pvr_cb, SPG_LOAD) {
  struct pvr *pvr = dc->pvr;

  pvr->SPG_LOAD->full = value;

  pvr_reconfigure_spg(pvr);
}

REG_W32(pvr_cb, FB_R_CTRL) {
  struct pvr *pvr = dc->pvr;

  pvr->FB_R_CTRL->full = value;

  pvr_reconfigure_spg(pvr);
}
