/*
 * Renesas R-Car Display Unit (Gen1, R8A7778/R8A7779) —
 * model for the Clarion QY8XXX board.
 *
 * The block is identified by its register magic values, not address
 * similarity: firmware writes 0x66000001 to +0xe0 (DEFR5_CODE = 0x66 << 24),
 * 0x77780010 to +0xe8 (DEFR6_CODE = 0x7778 << 16), and 0x77730011 to +0x20
 * (DEFR_CODE 0x7773), matching drivers/gpu/drm/renesas/rcar-du/rcar_du_regs.h.
 * Node in r8a7779.dtsi: display@fff80000 reg = <0xfff80000 0x40000>,
 * interrupts = <GIC_SPI 31>.
 *
 * Block layout (from rcar_du_regs.h and Linux driver accesses in
 * rcar_du_crtc.c / rcar_du_group.c / rcar_du_plane.c):
 *
 *   +0x00000  channel 0: DSYSR..DEWR, DOOR/BPOR, DEFR5/DDLTR/DEFR6 (CRTC 0)
 *   +0x00100  group 0 planes 1..8, stride PLANE_OFF = 0x100
 *   +0x01000  group 0 palettes CP1..CP4 (256 words each)
 *   +0x0a100  group 0 "A" planes
 *   +0x10000  ESCR02 / OTAR02, channel 0 sync registers
 *   +0x11000  DORCR / DPTSR / DAPTSR, +0x11020 DS1PR, +0x11024 DS2PR
 *   +0x30000  channel 1: same map as channel 0 (DU1_REG_OFFSET)
 *   +0x31000  ESCR13 / OTAR13, channel 1 sync registers
 *
 * The board bootloader enables 800x480 (full frame 1055x524) on both channels,
 * DOOR = 0, DORCR = PG1T|DK1S|PG1D_DS1, and channel 0 DSYSR = DEN; channel 1
 * remains disabled. ddi_ncg.dll programs planes later in WinCE user mode.
 * Guest programming of P1/P2/P8 and diagnostic rendering of injected frames
 * have been observed, but do not establish physical QY8 DU behavior or scanout.
 *
 * ⚠ Scope: plane rendering, DPPR and register programming follow
 * rcar_du_regs.h and the Linux driver. Diagnostic color-key and alpha rules
 * follow Linux v6.6 encoding (blend switch); ABIT_1 is inferred. These results
 * were checked only in QEMU diagnostic captures. Color-key/alpha and related
 * DU behavior have NOT been verified on physical QY8 hardware. Unverified
 * behavior is marked near its implementation.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/sysbus.h"
#include "hw/display/clarion_du.h"
#include "qemu/timer.h"
#include "trace.h"
#include "hw/core/cpu.h"
#include "ui/console.h"
#include "ui/pixel_ops.h"
#include "system/address-spaces.h"
#include "qom/object.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"

/* --- registers (names and offsets from rcar_du_regs.h) ----------------- */

#define DU0_REG_OFFSET      0x00000
#define DU1_REG_OFFSET      0x30000
#define DU_NUM_CHAN         2

#define DSYSR               0x00000
#define DSYSR_DRES          (1 << 9)
#define DSYSR_DEN           (1 << 8)

#define DSSR                0x00008     /* read-only status */
#define DSRCR               0x0000c     /* clear status bits */
#define DIER                0x00010     /* interrupt enable */

/*
 * Bit 11 in DSSR/DIER is frame synchronization (vertical blanking). Names
 * and bit number come from rcar_du_regs.h (`DSSR_VBK`, `DIER_VBE`) and match
 * both sides: the HU driver writes 0x800 to DIER, while its interrupt handler
 * (ddi_ncg.dll+0x12e78) reads DSSR, tests `tst r3, #0x800`, and clears it by
 * writing 0x800 to DSRCR.
 */
#define DSSR_VBK            (1 << 11)
#define DIER_VBE            (1 << 11)

#define HCR                 0x00050     /* full frame width - 1 */
#define VCR                 0x00058     /* full frame height - 1 */

#define DPPR                0x00018     /* plane priority and selection */
/*
 * Each DPPR nibble is priority level n = 1..8 (nibble n-1):
 *   DPE(n)    = 1 << (n*4 - 1)        - nibble bit 3, "level enabled"
 *   DPS(n, p) = (p - 1) << (n-1)*4    - bits 2..0, plane number 1..8
 * (rcar_du_regs.h). The bootloader writes 0x76543210: all eight levels have
 * plane assignments, but no DPE bits are set, so none is enabled.
 */
#define DPPR_DPE(n)         (1u << ((n) * 4 - 1))
#define DPPR_NIB(v, n)      (((v) >> (((n) - 1) * 4)) & 0xf)

/* Timing generator */
#define HDSR                0x00040
#define HDER                0x00044
#define VDSR                0x00048
#define VDER                0x0004c

/* Output attributes. DOOR is the color when the display is off; BPOR is the
 * background color under all planes (rcar_du_crtc.c: "Set display off and
 * background to black"). Both use 8 bits per channel: (r<<18)|(g<<10)|(b<<2). */
#define DOOR                0x00090
#define BPOR                0x00098

/* Group planes: plane n (1..8) is at group + n * PLANE_OFF */
#define PLANE_OFF           0x00100
#define DU_NUM_PLANES       8

#define PnMR                0x000
#define PnMR_CPSL_SHIFT     8
#define PnMR_SPIM_SHIFT     12
#define PnMR_SPIM_MASK      (3u << PnMR_SPIM_SHIFT)
#define PnMR_SPIM_TP         (0u << PnMR_SPIM_SHIFT)
#define PnMR_SPIM_ALP        (1u << PnMR_SPIM_SHIFT)
#define PnMR_SPIM_EOR        (2u << PnMR_SPIM_SHIFT)
#define PnMR_SPIM_TP_OFF     (1u << 14)
#define PnMR_TC              (1u << 17)
#define PnMR_WAE             (1u << 16)
#define PnMR_DDDF_MASK      3
#define PnMR_DDDF_8BPP      0
#define PnMR_DDDF_16BPP     1
#define PnMR_DDDF_ARGB      2
#define PnMR_DDDF_YC        3
#define PnMWR               0x004       /* row stride in PIXELS */
#define PnDSXR              0x010
#define PnDSYR              0x014
#define PnDPXR              0x018
#define PnDPYR              0x01c
#define PnDSA0R             0x020
#define PnDSA_MASK          0xfffffff0
#define PnALPHAR            0x008
#define PnALPHAR_ALPHA_MASK 0xff
#define PnALPHAR_ABIT_SHIFT 12
#define PnALPHAR_ABIT_MASK  (3u << PnALPHAR_ABIT_SHIFT)
#define PnALPHAR_ABIT_1     (0u << PnALPHAR_ABIT_SHIFT)
#define PnALPHAR_ABIT_0     (1u << PnALPHAR_ABIT_SHIFT)
#define PnALPHAR_ABIT_X     (2u << PnALPHAR_ABIT_SHIFT)
#define PnTC1R              0x044
#define PnTC2R              0x048
#define PnTC3R              0x04c
#define PnSPXR              0x030
#define PnSPYR              0x034
#define PnSWAPR             0x080
#define PnDDCR4             0x090
#define PnDDCR4_EDF_MASK    7
#define PnDDCR4_EDF_NONE    0
#define PnDDCR4_EDF_ARGB8888 1
#define PnDDCR4_EDF_RGB888  2
#define PnDDCR4_EDF_RGB666  3

/* Group palettes */
#define CP1_000R            0x01000
#define CP_STRIDE           0x01000

/* Dual-output control (group registers) */
#define DORCR               0x11000
#define DORCR_PG0D_MASK     (3 << 16)
#define DORCR_PG0D_DS0      (0 << 16)
#define DORCR_PG0D_DS1      (1 << 16)
#define DORCR_PG0D_FIX0     (2 << 16)
#define DORCR_PG0D_DOOR     (3 << 16)
#define DPTSR               0x11004
#define DPTSR_PnTS(n)       (1 << (n))  /* n is a zero-based plane index */
#define DS1PR               0x11020     /* composition of superposition 0 */
#define DS2PR               0x11024     /* composition of superposition 1 */

/* --- state ------------------------------------------------------------- */

OBJECT_DECLARE_SIMPLE_TYPE(ClarionDuState, CLARION_DU)

struct ClarionDuState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    QemuConsole *con;

    uint32_t *reg;              /* entire block as an array of words */
    int cols, rows;
    bool invalidate;

    qemu_irq irq;               /* GIC_SPI 31 */
    QEMUTimer *vbk;             /* frame clock */
    uint32_t dotclk;            /* dot clock in Hz (property) */
    uint32_t panel_width;
    uint32_t panel_height;

    /* avoid repeating identical warnings in the log */
    bool warned_chan1;
    bool warned_yc;
    bool warned_swap;
    bool warned_dppr;
    bool warned_bpp;
    bool warned_du_semantics;
    bool blend;
};

static inline uint32_t du_rd(ClarionDuState *s, hwaddr off)
{
    return s->reg[off / 4];
}

static inline hwaddr du_chan_base(int ch)
{
    return ch ? DU1_REG_OFFSET : DU0_REG_OFFSET;
}

/* DOOR/BPOR/palette -> 0x00RRGGBB */
static inline uint32_t du_color(uint32_t v)
{
    return (((v >> 18) & 0xff) << 16) |
           (((v >> 10) & 0xff) << 8) |
           ((v >> 2) & 0xff);
}

/*
 * Derive frame size from the timing generator: width = HDER - HDSR and
 * height = VDER - VDSR. The bootloader sets 984-184 = 800 and 511-31 = 480.
 */
static bool du_geometry(ClarionDuState *s, int ch, int *w, int *h)
{
    hwaddr b = du_chan_base(ch);
    int32_t cw = (int32_t)du_rd(s, b + HDER) - (int32_t)du_rd(s, b + HDSR);
    int32_t chh = (int32_t)du_rd(s, b + VDER) - (int32_t)du_rd(s, b + VDSR);

    if (cw <= 0 || chh <= 0 || cw > 4096 || chh > 4096) {
        return false;
    }
    if (s->panel_width) {
        cw = MIN(cw, (int32_t)s->panel_width);
    }
    if (s->panel_height) {
        chh = MIN(chh, (int32_t)s->panel_height);
    }
    *w = cw;
    *h = chh;
    return true;
}

/* --- planes (UNVERIFIED: the guest does not program them yet) ----------- */

typedef enum {
    DU_FMT_PAL8,
    DU_FMT_RGB565,
    DU_FMT_ARGB1555,
    DU_FMT_XRGB8888,
    DU_FMT_ARGB8888,
    DU_FMT_UNSUPPORTED,
} DuFormat;

static DuFormat du_plane_format(ClarionDuState *s, int plane, int *bpp)
{
    hwaddr pb = plane * PLANE_OFF;
    uint32_t pnmr = du_rd(s, pb + PnMR);
    uint32_t edf = du_rd(s, pb + PnDDCR4) & PnDDCR4_EDF_MASK;

    /*
     * PnMR.DDDF distinguishes 8bpp / "16 or 32 bits" / ARGB1555 / YC; EDF
     * in PnDDCR4 selects which of the 16/32-bit formats (rcar_du_kms.c table
     * rcar_du_format_infos: RGB565 = DDDF_16BPP + EDF_NONE, XRGB8888 =
     * DDDF_16BPP + EDF_RGB888, ARGB8888 = DDDF_16BPP + EDF_ARGB8888).
     * The driver also writes PnDDCR4 on Gen1: gen < 3 uses
     * rcar_du_plane_setup_format_gen2().
     */
    switch (pnmr & PnMR_DDDF_MASK) {
    case PnMR_DDDF_8BPP:
        *bpp = 1;
        return DU_FMT_PAL8;
    case PnMR_DDDF_ARGB:
        *bpp = 2;
        return DU_FMT_ARGB1555;
    case PnMR_DDDF_16BPP:
        switch (edf) {
        case PnDDCR4_EDF_ARGB8888:
            *bpp = 4;
            return DU_FMT_ARGB8888;
        case PnDDCR4_EDF_RGB888:
            *bpp = 4;
            return DU_FMT_XRGB8888;
        case PnDDCR4_EDF_NONE:
            *bpp = 2;
            return DU_FMT_RGB565;
        default:
            break;
        }
        break;
    default:
        break;
    }

    *bpp = 0;
    return DU_FMT_UNSUPPORTED;
}

static uint32_t du_decode_pixel(ClarionDuState *s, DuFormat fmt,
                                const uint8_t *p, uint32_t palette_base)
{
    uint32_t v;

    switch (fmt) {
    case DU_FMT_PAL8:
        return du_color(du_rd(s, palette_base + p[0] * 4));
    case DU_FMT_RGB565:
        v = p[0] | (p[1] << 8);
        return (((v >> 11) & 0x1f) * 255 / 31) << 16 |
               (((v >> 5) & 0x3f) * 255 / 63) << 8 |
               ((v & 0x1f) * 255 / 31);
    case DU_FMT_ARGB1555:
        v = p[0] | (p[1] << 8);
        return (((v >> 10) & 0x1f) * 255 / 31) << 16 |
               (((v >> 5) & 0x1f) * 255 / 31) << 8 |
               ((v & 0x1f) * 255 / 31);
    case DU_FMT_XRGB8888:
    case DU_FMT_ARGB8888:
        /* Word in memory is little-endian: 0xAARRGGBB. */
        return p[0] | (p[1] << 8) | (p[2] << 16);
    default:
        return 0;
    }
}

/*
 * List planes in a superposition from bottom to top.
 *
 * Source is DS1PR (superposition 0) / DS2PR (superposition 1): the driver
 * (rcar_du_crtc_update_planes) stores one nibble per plane, with values 1..8
 * for a plane and 0 for empty. The LOWEST nibble is the topmost layer (priority
 * counts down from the number of planes), so scanning high to low nibbles
 * gives bottom-to-top order.
 *
 * If DS1PR/DS2PR are empty, use DPPR, as the board's ddi_ncg.dll driver does.
 */
static int du_plane_list(ClarionDuState *s, int sp, int *order)
{
    uint32_t dspr = du_rd(s, sp ? DS2PR : DS1PR);
    uint32_t dptsr = du_rd(s, DPTSR);
    int n = 0;

    for (int nib = 7; nib >= 0; nib--) {
        int plane = (dspr >> (nib * 4)) & 0xf;

        if (plane < 1 || plane > DU_NUM_PLANES) {
            continue;
        }
        /* DPTSR: 0 -> plane feeds superposition 0; 1 -> superposition 1 */
        if (!!(dptsr & DPTSR_PnTS(plane - 1)) != !!sp) {
            continue;
        }
        order[n++] = plane;
    }

    /*
     * Fallback: DPPR.
     *
     * The Linux driver does not use this register, so it was not interpreted
     * previously. The board's ddi_ncg.dll uses it: DS1PR and DS2PR stay zero,
     * while DPPR changes 0x76543210 -> 0x76543218, setting DPE at level 1
     * with DPS = 0 (plane 1). This was observed during live boot; plane 1 is
     * fully programmed (800x480, ARGB1555, PnDSA0R = 0x10119400), and its
     * buffer contains nonzero pixels.
     *
     * Use the same order as DS1PR: scan from highest to lowest level so the
     * lower level is drawn on top.
     */
    if (!n) {
        uint32_t dppr = du_rd(s, DPPR);

        for (int lvl = DU_NUM_PLANES; lvl >= 1; lvl--) {
            int plane;

            if (!(dppr & DPPR_DPE(lvl))) {
                continue;               /* level disabled */
            }
            plane = (int)(DPPR_NIB(dppr, lvl) & 7) + 1;
            if (!!(dptsr & DPTSR_PnTS(plane - 1)) != !!sp) {
                continue;
            }
            order[n++] = plane;
        }
    }
    return n;
}

/* Draw one plane over an already rendered frame row. */
static void du_draw_plane(ClarionDuState *s, int plane,
                          uint32_t *fb, int cols, int rows)
{
    hwaddr pb = plane * PLANE_OFF;
    int bpp;
    DuFormat fmt = du_plane_format(s, plane, &bpp);
    uint32_t dsa = du_rd(s, pb + PnDSA0R) & PnDSA_MASK;
    uint32_t pnmr = du_rd(s, pb + PnMR);
    uint32_t spim = pnmr & PnMR_SPIM_MASK;
    uint32_t alphar = du_rd(s, pb + PnALPHAR);
    uint32_t tc2 = du_rd(s, pb + PnTC2R);
    uint32_t mwr = du_rd(s, pb + PnMWR);        /* row stride in pixels */
    int dsx = du_rd(s, pb + PnDSXR);
    int dsy = du_rd(s, pb + PnDSYR);
    int dpx = du_rd(s, pb + PnDPXR);
    int dpy = du_rd(s, pb + PnDPYR);
    int spx = du_rd(s, pb + PnSPXR);
    int spy = du_rd(s, pb + PnSPYR);
    uint32_t palette;
    g_autofree uint8_t *line = NULL;

    if (fmt == DU_FMT_UNSUPPORTED) {
        if (!s->warned_yc) {
            s->warned_yc = true;
            qemu_log_mask(LOG_UNIMP, "clarion-du: plane %d has YC/unknown "
                          "format (PnMR %#x, PnDDCR4 %#x); not rendering\n",
                          plane, du_rd(s, pb + PnMR), du_rd(s, pb + PnDDCR4));
        }
        return;
    }
    /* Fields/modes below are supported only by Linux v6.6 driver encoding;
     * this does not verify QY8 silicon behavior. */
    if (!s->warned_du_semantics &&
        ((pnmr & (PnMR_TC | PnMR_WAE)) || spim == PnMR_SPIM_EOR ||
         ((pnmr & PnMR_DDDF_MASK) == PnMR_DDDF_8BPP) ||
         fmt == DU_FMT_XRGB8888 || fmt == DU_FMT_ARGB8888)) {
        s->warned_du_semantics = true;
        qemu_log_mask(LOG_UNIMP, "clarion-du: some DU semantics "
                      "(TC/WAE/EOR, 8/32bpp key) are not modeled\n");
    }
    if (du_rd(s, pb + PnSWAPR) && !s->warned_swap) {
        s->warned_swap = true;
        qemu_log_mask(LOG_UNIMP, "clarion-du: plane %d PnSWAPR = %#x; "
                      "byte swapping is not modeled\n",
                      plane, du_rd(s, pb + PnSWAPR));
    }
    if (!dsa || dsx <= 0 || dsy <= 0 || mwr == 0) {
        return;
    }

    /* PnMR.CPSL selects the group palette CP1..CP4. */
    palette = CP1_000R + ((du_rd(s, pb + PnMR) >> PnMR_CPSL_SHIFT) & 3)
              * CP_STRIDE;

    line = g_malloc(dsx * (size_t)bpp);

    for (int y = 0; y < dsy; y++) {
        int dy = dpy + y;
        hwaddr src;
        uint32_t *dst;

        if (dy < 0 || dy >= rows) {
            continue;
        }
        /*
         * UNVERIFIED. PnMWR is a pixel stride; PnSPXR/PnSPYR are source
         * offsets (pixel / scanline). The Linux driver doubles SPY for 32bpp
         * "according to R8A7790 documentation"; SPY = 0 on a fullscreen plane,
         * so use the direct interpretation and leave this question open.
         */
        src = dsa + (hwaddr)(spy + y) * mwr * bpp + (hwaddr)spx * bpp;
        if (address_space_read(&address_space_memory, src,
                               MEMTXATTRS_UNSPECIFIED, line,
                               dsx * (size_t)bpp) != MEMTX_OK) {
            return;
        }

        dst = fb + (size_t)dy * cols;
        for (int x = 0; x < dsx; x++) {
            int dx = dpx + x;

            if (dx < 0 || dx >= cols) {
                continue;
            }
            const uint8_t *pixel = line + (size_t)x * bpp;
            uint32_t src = du_decode_pixel(s, fmt, pixel, palette);

            if (s->blend && !(pnmr & PnMR_SPIM_TP_OFF) &&
                (fmt == DU_FMT_ARGB1555 || fmt == DU_FMT_RGB565)) {
                /* Follows Linux v6.6 driver; not verified on physical QY8
                 * hardware. Key behavior with SPIM=ALP remains unresolved. */
                uint16_t raw = pixel[0] | ((uint16_t)pixel[1] << 8);
                uint16_t key_mask = fmt == DU_FMT_ARGB1555 ? 0x7fff : 0xffff;
                if ((raw & key_mask) == (tc2 & key_mask)) {
                    continue;
                }
            }

            if (s->blend && fmt == DU_FMT_ARGB1555 &&
                spim == PnMR_SPIM_ALP) {
                /* Follows Linux v6.6 driver, not verified on physical QY8:
                 * PnALPHAR coefficient and ABIT_0; ABIT_1 is inferred by
                 * symmetry. */
                uint32_t abit = alphar & PnALPHAR_ABIT_MASK;
                bool a_bit = !!(pixel[1] & 0x80);
                bool blend_pixel =
                    (abit == PnALPHAR_ABIT_X) ||
                    (abit == PnALPHAR_ABIT_1 && a_bit) ||
                    (abit == PnALPHAR_ABIT_0 && !a_bit);
                if (blend_pixel) {
                    uint32_t a = alphar & PnALPHAR_ALPHA_MASK;
                    uint32_t dstc = dst[dx];
                    uint32_t r = (((src >> 16) & 0xff) * a +
                                  ((dstc >> 16) & 0xff) * (255 - a)) / 255;
                    uint32_t g = (((src >> 8) & 0xff) * a +
                                  ((dstc >> 8) & 0xff) * (255 - a)) / 255;
                    uint32_t b = ((src & 0xff) * a +
                                  (dstc & 0xff) * (255 - a)) / 255;
                    src = (r << 16) | (g << 8) | b;
                }
            }
            dst[dx] = src;
        }
    }
}

/* --- output to the display window ------------------------------------- */

static bool clarion_du_gfx_update(void *opaque)
{
    ClarionDuState *s = opaque;
    DisplaySurface *surface = qemu_console_surface(s->con);
    int order[DU_NUM_PLANES];
    int w, h, nplanes, sp, src_ch;
    uint32_t dorcr, bg;
    g_autofree uint32_t *fb = NULL;
    uint8_t *dest;

    /* Nothing to display until the bootloader sets timing registers. */
    if (!du_geometry(s, 0, &w, &h)) {
        return true;
    }
    if (w != s->cols || h != s->rows) {
        qemu_console_resize(s->con, w, h);
        surface = qemu_console_surface(s->con);
        s->cols = w;
        s->rows = h;
        s->invalidate = true;
    }
    if (surface_bits_per_pixel(surface) != 32) {
        if (!s->warned_bpp) {
            s->warned_bpp = true;
            qemu_log_mask(LOG_UNIMP, "clarion-du: surface uses %d bits per "
                          "pixel; model supports only 32\n",
                          surface_bits_per_pixel(surface));
        }
        return true;
    }

    /*
     * DORCR.PG0D selects the DPAD0 output, the only output connected to the
     * panel on this board: superposition 0, superposition 1, constant zero,
     * or the DOOR color.
     */
    dorcr = du_rd(s, DORCR);
    switch (dorcr & DORCR_PG0D_MASK) {
    case DORCR_PG0D_DS1:
        sp = 1;
        src_ch = 1;
        break;
    case DORCR_PG0D_FIX0:
    case DORCR_PG0D_DOOR:
        sp = -1;
        src_ch = 0;
        break;
    default:
        sp = 0;
        src_ch = 0;
        break;
    }

    if (!(du_rd(s, du_chan_base(src_ch) + DSYSR) & DSYSR_DEN)) {
        /* Display is off; output the DOOR color (rcar_du_crtc.c). */
        sp = -1;
    }

    if (sp < 0) {
        bg = ((dorcr & DORCR_PG0D_MASK) == DORCR_PG0D_FIX0)
             ? 0 : du_color(du_rd(s, du_chan_base(src_ch) + DOOR));
        nplanes = 0;
    } else {
        bg = du_color(du_rd(s, du_chan_base(src_ch) + BPOR));
        nplanes = du_plane_list(s, sp, order);
    }

    /* Without planes, the frame is constant; redraw only when it changes. */
    if (!nplanes && !s->invalidate) {
        return true;
    }

    fb = g_new(uint32_t, (size_t)w * h);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        fb[i] = bg;
    }
    for (int i = 0; i < nplanes; i++) {
        du_draw_plane(s, order[i], fb, w, h);
    }

    dest = surface_data(surface);
    for (int y = 0; y < h; y++) {
        uint32_t *row = (uint32_t *)(dest + (size_t)y * surface_stride(surface));

        for (int x = 0; x < w; x++) {
            uint32_t c = fb[(size_t)y * w + x];

            row[x] = rgb_to_pixel32((c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff);
        }
    }

    qemu_console_update(s->con, 0, 0, w, h);
    s->invalidate = false;
    return true;
}

static void clarion_du_invalidate(void *opaque)
{
    ClarionDuState *s = opaque;

    s->invalidate = true;
}

static const GraphicHwOps clarion_du_gfx_ops = {
    .invalidate = clarion_du_invalidate,
    .gfx_update = clarion_du_gfx_update,
};

/* --- MMIO ------------------------------------------------------------- */

static uint64_t clarion_du_read(void *opaque, hwaddr addr, unsigned size)
{
    ClarionDuState *s = opaque;

    /*
     * DSSR (frame status: VBK, HBK, FRM, ...) reads as zero.
     *
     * The bootloader sets DIER = DIER_VBE, enabling frame-sync interrupts
     * (GIC_SPI 31 in r8a7779.dtsi). Generating them requires the dot clock,
     * which is not in the DU registers: ESCR02 is 0, so DCLKSEL = DCLKIN uses
     * an external clock whose frequency is unknown. We do not guess it, so
     * the model has neither a frame counter nor an interrupt line. Open
     * question: does the user-mode driver wait for VBK?
     */
    if ((addr & 0xffff) == DSSR) {
        /*
         * Count frame-status reads to distinguish "driver does not use frame
         * sync" from "driver waits for it but the model is silent". The
         * bootloader enables DIER=VBK at 917 ms.
         */
        static uint32_t dssr_reads;
        trace_clarion_du_dssr_read(
            (uint32_t)(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / SCALE_MS),
            (uint32_t)addr, ++dssr_reads);
    }

    return s->reg[addr / 4];
}

/*
 * Register name for logging. Plane names (`P1DSA0R`, etc.) are constructed
 * dynamically: plane n is at group + n * PLANE_OFF, so derive n by division.
 */
static const char *du_reg_name(hwaddr addr)
{
    static char buf[16];
    hwaddr off = addr & 0xffff;

    switch (off) {
    case DSYSR:   return "DSYSR";
    case DSSR:    return "DSSR";
    case DSRCR:   return "DSRCR";
    case DIER:    return "DIER";
    case DPPR:    return "DPPR";
    case DOOR:    return "DOOR";
    case BPOR:    return "BPOR";
    case HDSR:    return "HDSR";
    case HDER:    return "HDER";
    case VDSR:    return "VDSR";
    case VDER:    return "VDER";
    case DORCR:   return "DORCR";
    case DPTSR:   return "DPTSR";
    case DS1PR:   return "DS1PR";
    case DS2PR:   return "DS2PR";
    default:      break;
    }

    if (off >= PLANE_OFF && off < (DU_NUM_PLANES + 1) * PLANE_OFF) {
        int plane = off / PLANE_OFF;
        hwaddr po = off % PLANE_OFF;
        const char *r = NULL;

        switch (po) {
        case PnMR:     r = "MR";     break;
        case PnMWR:    r = "MWR";    break;
        case PnALPHAR: r = "ALPHAR"; break;
        case PnTC1R:   r = "TC1R";   break;
        case PnTC2R:   r = "TC2R";   break;
        case PnTC3R:   r = "TC3R";   break;
        case 0x084:    r = "DDCR";   break;
        case 0x088:    r = "DDCR2";  break;
        case PnDSXR:   r = "DSXR";   break;
        case PnDSYR:   r = "DSYR";   break;
        case PnDPXR:   r = "DPXR";   break;
        case PnDPYR:   r = "DPYR";   break;
        case PnDSA0R:  r = "DSA0R";  break;
        case PnSPXR:   r = "SPXR";   break;
        case PnSPYR:   r = "SPYR";   break;
        case PnSWAPR:  r = "SWAPR";  break;
        case PnDDCR4:  r = "DDCR4";  break;
        default:       break;
        }
        if (r) {
            snprintf(buf, sizeof(buf), "P%d%s", plane, r);
            return buf;
        }
    }

    snprintf(buf, sizeof(buf), "?%05x", (unsigned)off);
    return buf;
}

/* --- frame synchronization (VBK) --------------------------------------- */

/*
 * The interrupt line stays asserted while any enabled status bit is set.
 * The driver clears it by writing to DSRCR, which drops the line; the model
 * needs no separate acknowledgment.
 */
static void du_irq_update(ClarionDuState *s)
{
    qemu_set_irq(s->irq, !!(du_rd(s, DSSR) & du_rd(s, DIER)));
}

/*
 * Derive frame period from the REGISTERS programmed by the guest:
 * full frame = (HCR + 1) x (VCR + 1) pixels. On the HU this is 1056 x 525.
 *
 * The only unavailable value is the dot clock: ESCR02 remains 0, so
 * DCLKSEL = DCLKIN, an external clock from TCON (PA 0xFFF18000). Its source
 * frequency is unknown, so machine property `dotclk` defaults to 33,333,333
 * Hz, the R-Car M1A reference EXTAL. For a 1056 x 525 frame this gives 60.1 Hz.
 */
static void du_vbk_resched(ClarionDuState *s)
{
    uint64_t total = (uint64_t)(du_rd(s, HCR) + 1) * (du_rd(s, VCR) + 1);
    bool on = (du_rd(s, DSYSR) & DSYSR_DEN) && (du_rd(s, DIER) & DIER_VBE);

    if (!on || total < 2 || !s->dotclk) {
        timer_del(s->vbk);
        return;
    }
    timer_mod_ns(s->vbk, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                 muldiv64(total, NANOSECONDS_PER_SECOND, s->dotclk));
}

static void du_vbk_tick(void *opaque)
{
    ClarionDuState *s = opaque;

    s->reg[DSSR / 4] |= DSSR_VBK;
    du_irq_update(s);
    du_vbk_resched(s);
}

static void clarion_du_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    ClarionDuState *s = opaque;
    hwaddr off = addr & 0xffff;

    /*
     * Log before processing DSSR/DSRCR: otherwise DSRCR writes (which clear
     * status bits in the interrupt handler) would be lost to an early return
     * and appear not to have occurred.
     */
    if (off == DSSR || off == DSRCR || s->reg[addr / 4] != (uint32_t)val) {
        /*
         * Log register changes to identify when the image changes and which
         * register caused it. Record only actual changes: the driver rewrites
         * identical values each frame, which would otherwise flood the trace.
         * Time is virtual milliseconds, matching guest console timestamps.
         */
        CPUState *cs = current_cpu;
        uint64_t pc = 0;

        /*
         * Guest PC. TCG updates it at translation-block boundaries, so this
         * is accurate to the block, not the `str` instruction. That suffices
         * to identify the writing module because XIP module bases are tens of
         * kilobytes apart.
         */
        if (cs) {
            pc = CPU_GET_CLASS(cs)->get_pc(cs);
        }
        trace_clarion_du_reg_write(
            (uint32_t)(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / SCALE_MS),
            du_reg_name(addr), (uint32_t)addr,
            s->reg[addr / 4], (uint32_t)val, pc);
    }

    if (off == DSSR) {
        return;                 /* status is read-only */
    }
    if (off == DSRCR) {
        s->reg[DSSR / 4] &= ~(uint32_t)val;
        du_irq_update(s);
        return;
    }

    s->reg[addr / 4] = val;

    /* All registers affecting channel 0 frame timing. */
    if (addr == DSYSR || addr == DIER || addr == HCR || addr == VCR) {
        du_vbk_resched(s);
        if (addr == DIER) {
            du_irq_update(s);
        }
    }

    if (addr == DU1_REG_OFFSET + DSYSR && (val & DSYSR_DEN) &&
        !s->warned_chan1) {
        s->warned_chan1 = true;
        qemu_log_mask(LOG_UNIMP, "clarion-du: channel 1 (DU1) enabled; "
                      "only DPAD0 is displayed\n");
    }

    s->invalidate = true;
}

static const MemoryRegionOps clarion_du_ops = {
    .read = clarion_du_read,
    .write = clarion_du_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void clarion_du_reset_hold(Object *obj, ResetType type)
{
    ClarionDuState *s = CLARION_DU(obj);

    memset(s->reg, 0, CLARION_DU_SIZE);
    s->cols = 0;
    s->rows = 0;
    s->invalidate = true;
    if (s->vbk) {
        timer_del(s->vbk);
    }
    qemu_set_irq(s->irq, 0);
}

static void clarion_du_realize(DeviceState *dev, Error **errp)
{
    ClarionDuState *s = CLARION_DU(dev);

    s->reg = g_malloc0(CLARION_DU_SIZE);
    memory_region_init_io(&s->iomem, OBJECT(dev), &clarion_du_ops, s,
                          "clarion-du", CLARION_DU_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    s->vbk = timer_new_ns(QEMU_CLOCK_VIRTUAL, du_vbk_tick, s);

    s->con = qemu_graphic_console_create(dev, 0, &clarion_du_gfx_ops, s);
}

static const Property clarion_du_props[] = {
    /* Diagnostic renderer switch; Linux v6.6 field semantics are not QY8 verified. */
    DEFINE_PROP_BOOL("blend", ClarionDuState, blend, true),
    /*
     * DCLKIN dot clock in Hz; frame period = (HCR+1)*(VCR+1)/dotclk.
     * DU registers do not contain it: ESCR02 = 0 selects the external TCON
     * (PA 0xFFF18000).
     *
     * ⚠ Defaults to 0, so frame interrupts are NOT generated. The model is
     * not known to be wrong; the GIC line used by this firmware is UNKNOWN.
     * The driver uses hard-coded SYSINTR = 37, and probing all 17 lines
     * enabled by the OAL did not wake its handler thread. Until the line is
     * known, generating interrupts could target the wrong device.
     *
     * Once identified, set `-global clarion-du.dotclk=33333333` (R-Car M1A
     * reference EXTAL; 60.1 Hz for 1056 x 525) and
     * `-M clarion-qy8,du-spi=<number>`.
     */
    DEFINE_PROP_UINT32("dotclk", ClarionDuState, dotclk, 0),
    DEFINE_PROP_UINT32("panel-width", ClarionDuState, panel_width, 0),
    DEFINE_PROP_UINT32("panel-height", ClarionDuState, panel_height, 0),
};

static void clarion_du_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = clarion_du_realize;
    dc->desc = "Renesas R-Car Display Unit (Gen1)";
    device_class_set_props(dc, clarion_du_props);
    rc->phases.hold = clarion_du_reset_hold;
}

static const TypeInfo clarion_du_types[] = {
    {
        .name          = TYPE_CLARION_DU,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(ClarionDuState),
        .class_init    = clarion_du_class_init,
    },
};

DEFINE_TYPES(clarion_du_types)
