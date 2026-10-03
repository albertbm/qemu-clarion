/*
 * Cypress TrueTouch (TTSP) touch screen controllers on I2C
 *
 * Two flavours, both modelled from what the Clarion QY8 WinCE touch
 * drivers do on the bus:
 *
 *  cy-tma460 (QY8602NB, TouchPaneldrv_TMA460.dll): TTSP Gen4 as in Linux
 *    cyttsp4. One address (0x24); bootloader packets are written to
 *    register 0 behind a 0xFF byte; sysinfo mode publishes a map of
 *    offsets that the driver follows to find the touch report.
 *
 *  cy-tma616 (QY8202NA, TouchPaneldrv.dll): application on 0x67,
 *    bootloader on 0x69 taking bare packets, fixed register layout.
 *
 * Both drivers reset the chip through a GPIO, wait for an interrupt from
 * the bootloader, query it (0x38), leave it (0x3B), wait for the
 * application's interrupt, read sysinfo and switch to operating mode.
 * Every step that the driver waits for raises a short low pulse on INT.
 *
 * A QEMU absolute pointer drives one touch: left button down/up and
 * motion while it is held.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/input/cypress_ttsp.h"
#include "hw/i2c/i2c.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "ui/input.h"
#include "trace.h"

OBJECT_DECLARE_SIMPLE_TYPE(CypressTTSPState, CYPRESS_TTSP)

enum {
    TTSP_OFF,           /* held in reset */
    TTSP_BL,
    TTSP_SYSINFO,
    TTSP_OPERATING,
    TTSP_CONFIG,
};

enum {
    ACT_NONE,
    ACT_PULSE,          /* bootloader answer ready */
    ACT_BL_READY,       /* out of reset */
    ACT_APP_READY,      /* application started after 0x3B */
};

/* hst_mode bits */
#define HST_TOGGLE      0x80
#define HST_MODE_MASK   0x70
#define HST_MODE_OP     0x00
#define HST_MODE_SYSINFO 0x10
#define HST_MODE_CONFIG 0x20
#define HST_CHANGE      0x08
#define HST_RESET       0x01

#define CMD_OFS         2
#define CMD_COMPLETE    0x40

/* Gen4 maps; the TMA460 driver reads touches with a stride of 10 */
#define G4_CYDATA       0x10
#define G4_MFGID_SZ     8
#define G4_TEST         (G4_CYDATA + 26 + G4_MFGID_SZ)
#define G4_PCFG         (G4_TEST + 2)
#define G4_OPCFG        (G4_PCFG + 13)
#define G4_DDATA        (G4_OPCFG + 34)
#define G4_MDATA        (G4_DDATA + 16)
#define G4_MAP_SZ       (G4_MDATA + 16)
#define G4_REP          10
#define G4_TT_STAT      23
#define G4_TCH_REC      10
#define G4_MAX_TCH      10

#define TMA616_TCH      11      /* touch count; records follow, 7 bytes */

#define PULSE_NS        (100 * SCALE_US)

struct CypressTTSPState {
    I2CSlave parent_obj;

    qemu_irq int_out;
    QEMUTimer *pulse_timer;
    QEMUTimer *act_timer;
    QemuInputHandlerState *input;

    /* per model */
    bool gen4;
    uint8_t bl_addr;            /* TMA616: separate bootloader address */
    uint16_t width, height;     /* panel resolution reported to the host */

    uint32_t mode;
    uint32_t act;
    bool cur_bl;                /* this transfer addressed bl_addr */
    bool writing;
    bool have_off;
    uint8_t off;
    uint8_t wlen;
    uint8_t wbuf[256];
    uint8_t regs[256];
    uint8_t bl_resp[16];

    /* pointer state */
    bool btn, was_down;
    uint16_t x, y;
};

/* TMA616 parameter sizes, from the driver's block 0 table */
static const uint8_t tma616_param_size[0x3d] = {
    1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2,
    1, 2, 1, 1, 1, 1, 1, 2, 1, 1, 1, 1, 1, 1, 2, 1,
    2, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 1, 1, 2, 1,
    1, 1, 2, 2, 1, 1, 2, 2, 2, 2, 1, 2, 2,
};

static void ttsp_int_pulse(CypressTTSPState *s)
{
    trace_cypress_ttsp_pulse(s->mode);
    qemu_set_irq(s->int_out, 0);
    timer_mod(s->pulse_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + PULSE_NS);
}

static void ttsp_pulse_end(void *opaque)
{
    CypressTTSPState *s = opaque;

    qemu_set_irq(s->int_out, 1);
}

static void ttsp_later(CypressTTSPState *s, uint32_t act, int64_t us)
{
    s->act = act;
    timer_mod(s->act_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              us * SCALE_US);
}

static void put_be16(uint8_t *p, uint16_t v)
{
    p[0] = v >> 8;
    p[1] = v;
}

static void ttsp_build_sysinfo(CypressTTSPState *s)
{
    uint8_t *r = s->regs;

    r[0] = HST_MODE_SYSINFO;
    if (!s->gen4) {
        /* the TMA616 driver only checks hst_mode and the gesture bit */
        return;
    }
    put_be16(r + 0x02, G4_MAP_SZ);
    put_be16(r + 0x04, G4_CYDATA);
    put_be16(r + 0x06, G4_TEST);
    put_be16(r + 0x08, G4_PCFG);
    put_be16(r + 0x0a, G4_OPCFG);
    put_be16(r + 0x0c, G4_DDATA);
    put_be16(r + 0x0e, G4_MDATA);

    /* cydata: ttpid, fw version, ..., mfgid_sz at +18, mfg_id at +19 */
    put_be16(r + G4_CYDATA, 0x0460);
    r[G4_CYDATA + 2] = 1;
    r[G4_CYDATA + 12] = 1;
    r[G4_CYDATA + 18] = G4_MFGID_SZ;

    /* pcfg: electrodes, length, resolution, max z */
    r[G4_PCFG + 0] = 0x1d;
    r[G4_PCFG + 1] = 0x11;
    put_be16(r + G4_PCFG + 6, s->width);
    put_be16(r + G4_PCFG + 8, s->height);
    put_be16(r + G4_PCFG + 10, 255);

    /* opcfg: where the operating mode keeps its command and report */
    r[G4_OPCFG + 0] = CMD_OFS;
    r[G4_OPCFG + 1] = G4_REP;
    put_be16(r + G4_OPCFG + 2, G4_TT_STAT + 1 + G4_TCH_REC * G4_MAX_TCH -
             G4_REP);
    r[G4_OPCFG + 5] = G4_TT_STAT;
    r[G4_OPCFG + 7] = G4_MAX_TCH;
    r[G4_OPCFG + 8] = G4_TCH_REC;
    /* {loc, size}: x, y, p, t, e, o, w */
    r[G4_OPCFG + 9] = 0;   r[G4_OPCFG + 10] = 2;
    r[G4_OPCFG + 11] = 2;  r[G4_OPCFG + 12] = 2;
    r[G4_OPCFG + 13] = 4;  r[G4_OPCFG + 14] = 1;
    r[G4_OPCFG + 15] = 5;  r[G4_OPCFG + 16] = 1;
    r[G4_OPCFG + 17] = 5;  r[G4_OPCFG + 18] = 1;
    r[G4_OPCFG + 19] = 6;  r[G4_OPCFG + 20] = 1;
    r[G4_OPCFG + 21] = 7;  r[G4_OPCFG + 22] = 1;
}

static void ttsp_set_mode(CypressTTSPState *s, uint32_t mode, uint8_t hst)
{
    s->mode = mode;
    memset(s->regs, 0, sizeof(s->regs));
    if (mode == TTSP_SYSINFO) {
        ttsp_build_sysinfo(s);
    } else if (mode == TTSP_CONFIG) {
        s->regs[0] = HST_MODE_CONFIG;
    }
    /* keep the host's toggle and other non-mode bits */
    s->regs[0] |= hst & ~(HST_MODE_MASK | HST_CHANGE | HST_RESET);
}

static void ttsp_act(void *opaque)
{
    CypressTTSPState *s = opaque;
    uint32_t act = s->act;

    s->act = ACT_NONE;
    switch (act) {
    case ACT_BL_READY:
        s->mode = TTSP_BL;
        memset(s->bl_resp, 0, sizeof(s->bl_resp));
        break;
    case ACT_APP_READY:
        ttsp_set_mode(s, TTSP_SYSINFO, 0);
        break;
    case ACT_PULSE:
        break;
    default:
        return;
    }
    ttsp_int_pulse(s);
}

static void ttsp_bl_answer(CypressTTSPState *s, const uint8_t *data, int len)
{
    uint8_t *p = s->bl_resp;

    memset(p, 0, sizeof(s->bl_resp));
    p[0] = 0x01;                /* SOP, status 0 */
    p[2] = len;
    if (len) {
        memcpy(p + 4, data, len);
    }
    p[4 + len + 2] = 0x17;      /* EOP; the drivers don't check the CRC */
}

static void ttsp_bl_packet(CypressTTSPState *s, const uint8_t *pkt, int len)
{
    /* silicon id (4), revision, bootloader version */
    static const uint8_t info[8] = { 0x05, 0xa2, 0x11, 0x69, 0x01, 1, 0, 0 };

    if (len < 7 || pkt[0] != 0x01) {
        qemu_log_mask(LOG_GUEST_ERROR, "cypress-ttsp: bad bootloader packet\n");
        return;
    }
    trace_cypress_ttsp_bl_cmd(pkt[1]);
    switch (pkt[1]) {
    case 0x38:                  /* enter/get info */
        ttsp_bl_answer(s, info, sizeof(info));
        ttsp_later(s, ACT_PULSE, 200);
        break;
    case 0x3b:                  /* exit to the application */
        ttsp_bl_answer(s, NULL, 0);
        ttsp_later(s, ACT_APP_READY, 2000);
        break;
    default:
        ttsp_bl_answer(s, NULL, 0);
        ttsp_later(s, ACT_PULSE, 200);
        break;
    }
}

static void ttsp_command(CypressTTSPState *s)
{
    uint8_t *r = s->regs;
    uint8_t cmd = r[CMD_OFS] & 0x3f;

    trace_cypress_ttsp_command(cmd, s->mode);

    if (s->gen4 && cmd == 0x03 && s->mode == TTSP_CONFIG) {
        /* read config block: row, length, ebid in r[3..7] */
        unsigned len = MIN((r[5] << 8) | r[6], 128);
        uint8_t ebid = r[7];

        memset(r + 3, 0, sizeof(s->regs) - 3);
        r[4] = ebid;
        put_be16(r + 5, len);
        /* block size (LE) first; byte 29 bit 0 turns the handshake on */
        r[8] = 0x00;
        r[9] = 0x01;
        r[8 + 29] = 0x01;
    } else if (!s->gen4 && cmd == 0x02) {
        /* get parameter: id echoed, size, value */
        uint8_t id = r[3];

        memset(r + 3, 0, 7);
        r[3] = id;
        r[4] = id < sizeof(tma616_param_size) ? tma616_param_size[id] : 1;
    } else if (!s->gen4 && cmd == 0x26) {
        /* the driver expects the argument back at +6 */
        r[8] = r[3];
    }
    r[CMD_OFS] = CMD_COMPLETE | cmd;
    ttsp_int_pulse(s);
}

static void ttsp_hst_write(CypressTTSPState *s, uint8_t v)
{
    if (v & HST_CHANGE) {
        switch (v & HST_MODE_MASK) {
        case HST_MODE_OP:
            ttsp_set_mode(s, TTSP_OPERATING, v);
            break;
        case HST_MODE_SYSINFO:
            ttsp_set_mode(s, TTSP_SYSINFO, v);
            break;
        case HST_MODE_CONFIG:
            ttsp_set_mode(s, TTSP_CONFIG, v);
            break;
        default:
            s->regs[0] = v & ~HST_CHANGE;
            break;
        }
        ttsp_int_pulse(s);
    } else if (v & HST_RESET) {
        s->mode = TTSP_OFF;
        ttsp_later(s, ACT_BL_READY, 2000);
    } else {
        /* toggle bit: host acknowledges a report */
        s->regs[0] = v;
    }
}

static void ttsp_write_done(CypressTTSPState *s)
{
    int i;

    trace_cypress_ttsp_write(s->cur_bl, s->mode, s->off, s->wlen);

    s->writing = false;
    if (!s->have_off) {
        return;
    }
    if (s->cur_bl) {
        /* TMA616 bootloader: one byte sets the read pointer */
        if (s->wlen) {
            uint8_t pkt[257];

            pkt[0] = s->off;
            memcpy(pkt + 1, s->wbuf, s->wlen);
            ttsp_bl_packet(s, pkt, s->wlen + 1);
            s->off = 0;
        }
        return;
    }
    if (s->mode == TTSP_BL) {
        if (s->off == 0 && s->wlen > 1 && s->wbuf[0] == 0xff) {
            ttsp_bl_packet(s, s->wbuf + 1, s->wlen - 1);
        }
        return;
    }
    if (!s->wlen) {
        return;
    }
    for (i = 0; i < s->wlen; i++) {
        uint8_t reg = s->off + i;

        if (reg == 0) {
            ttsp_hst_write(s, s->wbuf[i]);
        } else {
            s->regs[reg] = s->wbuf[i];
        }
    }
    if (s->mode != TTSP_SYSINFO &&
        s->off <= CMD_OFS && s->off + s->wlen > CMD_OFS) {
        ttsp_command(s);
    }
    s->off += s->wlen;
}

static bool ttsp_match(I2CSlave *candidate, uint8_t address, bool broadcast,
                       I2CNodeList *current_devs)
{
    CypressTTSPState *s = CYPRESS_TTSP(candidate);
    bool bl = s->bl_addr && address == s->bl_addr;
    I2CNode *node;

    if (address != candidate->address && !bl && !broadcast) {
        return false;
    }
    s->cur_bl = bl;

    node = g_new(I2CNode, 1);
    node->elt = candidate;
    QLIST_INSERT_HEAD(current_devs, node, next);
    return true;
}

static int ttsp_event(I2CSlave *i2c, enum i2c_event event)
{
    CypressTTSPState *s = CYPRESS_TTSP(i2c);
    bool bl_mode = s->mode == TTSP_BL;

    switch (event) {
    case I2C_START_SEND:
    case I2C_START_RECV:
        if (s->writing) {
            ttsp_write_done(s);
        }
        /* nothing answers in reset; the two TMA616 addresses take turns */
        if (s->mode == TTSP_OFF ||
            (s->bl_addr && s->cur_bl != bl_mode)) {
            return 1;
        }
        if (event == I2C_START_SEND) {
            s->writing = true;
            s->have_off = false;
            s->wlen = 0;
        }
        break;
    case I2C_FINISH:
        if (s->writing) {
            ttsp_write_done(s);
        }
        break;
    default:
        break;
    }
    return 0;
}

static int ttsp_send(I2CSlave *i2c, uint8_t data)
{
    CypressTTSPState *s = CYPRESS_TTSP(i2c);

    if (!s->have_off) {
        s->off = data;
        s->have_off = true;
    } else if (s->wlen < sizeof(s->wbuf) - 1) {
        s->wbuf[s->wlen++] = data;
    }
    return 0;
}

static uint8_t ttsp_recv(I2CSlave *i2c)
{
    CypressTTSPState *s = CYPRESS_TTSP(i2c);
    uint8_t off = s->off++;

    if (s->mode == TTSP_BL) {
        return off < sizeof(s->bl_resp) ? s->bl_resp[off] : 0;
    }
    return s->regs[off];
}

static void ttsp_reset_in(void *opaque, int n, int level)
{
    CypressTTSPState *s = opaque;

    trace_cypress_ttsp_reset_line(level);
    if (!level) {
        s->mode = TTSP_OFF;
        s->act = ACT_NONE;
        timer_del(s->act_timer);
    } else if (s->mode == TTSP_OFF && s->act != ACT_BL_READY) {
        ttsp_later(s, ACT_BL_READY, 2000);
    }
}

/* One touch in the operating mode report, then INT. */
static void ttsp_report(CypressTTSPState *s, int event)
{
    uint8_t *r = s->regs;
    uint8_t *rec;

    if (s->gen4) {
        r[G4_REP + 1] = 0;
        r[G4_TT_STAT] = 1;
        rec = r + G4_TT_STAT + 1;
        memset(rec, 0, G4_TCH_REC);
        rec[5] = event << 4;
    } else {
        r[TMA616_TCH - 1] = 0;
        r[TMA616_TCH] = 1;
        rec = r + TMA616_TCH + 1;
        memset(rec, 0, 7);
        rec[5] = event << 5;
        rec[6] = 0x10;
    }
    trace_cypress_ttsp_report(event, s->x, s->y);
    put_be16(rec, s->x);
    put_be16(rec + 2, s->y);
    rec[4] = event == 3 ? 0 : 0x40;
    ttsp_int_pulse(s);
}

static void ttsp_input_event(DeviceState *dev, QemuConsole *src,
                             QemuInputEvent *evt)
{
    CypressTTSPState *s = CYPRESS_TTSP(dev);
    InputMoveEvent *move;
    InputBtnEvent *btn;

    switch (evt->type) {
    case INPUT_EVENT_KIND_ABS:
        move = &evt->abs;
        if (move->axis == INPUT_AXIS_X) {
            s->x = qemu_input_scale_axis(move->value, INPUT_EVENT_ABS_MIN,
                                         INPUT_EVENT_ABS_MAX, 0, s->width - 1);
        } else if (move->axis == INPUT_AXIS_Y) {
            s->y = qemu_input_scale_axis(move->value, INPUT_EVENT_ABS_MIN,
                                         INPUT_EVENT_ABS_MAX, 0,
                                         s->height - 1);
        }
        break;
    case INPUT_EVENT_KIND_BTN:
        btn = &evt->btn;
        if (btn->button == INPUT_BUTTON_LEFT) {
            s->btn = btn->down;
        }
        break;
    default:
        break;
    }
}

static void ttsp_input_sync(DeviceState *dev)
{
    CypressTTSPState *s = CYPRESS_TTSP(dev);

    if (s->mode != TTSP_OPERATING) {
        s->was_down = false;
        return;
    }
    if (s->btn) {
        ttsp_report(s, s->was_down ? 2 : 1);
    } else if (s->was_down) {
        ttsp_report(s, 3);
    }
    s->was_down = s->btn;
}

static const QemuInputHandler ttsp_input_handler = {
    .name  = "Cypress TrueTouch",
    .mask  = INPUT_EVENT_MASK_BTN | INPUT_EVENT_MASK_ABS,
    .event = ttsp_input_event,
    .sync  = ttsp_input_sync,
};

static void ttsp_reset(DeviceState *dev)
{
    CypressTTSPState *s = CYPRESS_TTSP(dev);

    timer_del(s->pulse_timer);
    timer_del(s->act_timer);
    /* powered with reset released, bootloader idle */
    s->mode = TTSP_BL;
    s->act = ACT_NONE;
    s->writing = false;
    s->have_off = false;
    s->btn = s->was_down = false;
    memset(s->regs, 0, sizeof(s->regs));
    memset(s->bl_resp, 0, sizeof(s->bl_resp));
    qemu_set_irq(s->int_out, 1);
}

static void ttsp_realize(DeviceState *dev, Error **errp)
{
    CypressTTSPState *s = CYPRESS_TTSP(dev);

    s->pulse_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, ttsp_pulse_end, s);
    s->act_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, ttsp_act, s);
    s->input = qemu_input_handler_register(dev, &ttsp_input_handler);
}

static void ttsp_init(Object *obj)
{
    DeviceState *dev = DEVICE(obj);
    CypressTTSPState *s = CYPRESS_TTSP(obj);

    qdev_init_gpio_in_named(dev, ttsp_reset_in, CYPRESS_TTSP_RESET, 1);
    qdev_init_gpio_out_named(dev, &s->int_out, CYPRESS_TTSP_INT, 1);
}

static const Property ttsp_props[] = {
    DEFINE_PROP_UINT16("width", CypressTTSPState, width, 800),
    DEFINE_PROP_UINT16("height", CypressTTSPState, height, 480),
};

static const VMStateDescription vmstate_ttsp = {
    .name = TYPE_CYPRESS_TTSP,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, CypressTTSPState),
        VMSTATE_UINT32(mode, CypressTTSPState),
        VMSTATE_UINT32(act, CypressTTSPState),
        VMSTATE_BOOL(cur_bl, CypressTTSPState),
        VMSTATE_BOOL(writing, CypressTTSPState),
        VMSTATE_BOOL(have_off, CypressTTSPState),
        VMSTATE_UINT8(off, CypressTTSPState),
        VMSTATE_UINT8(wlen, CypressTTSPState),
        VMSTATE_UINT8_ARRAY(wbuf, CypressTTSPState, 256),
        VMSTATE_UINT8_ARRAY(regs, CypressTTSPState, 256),
        VMSTATE_UINT8_ARRAY(bl_resp, CypressTTSPState, 16),
        VMSTATE_BOOL(btn, CypressTTSPState),
        VMSTATE_BOOL(was_down, CypressTTSPState),
        VMSTATE_UINT16(x, CypressTTSPState),
        VMSTATE_UINT16(y, CypressTTSPState),
        VMSTATE_TIMER_PTR(pulse_timer, CypressTTSPState),
        VMSTATE_TIMER_PTR(act_timer, CypressTTSPState),
        VMSTATE_END_OF_LIST()
    }
};

static void ttsp_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    dc->realize = ttsp_realize;
    device_class_set_legacy_reset(dc, ttsp_reset);
    device_class_set_props(dc, ttsp_props);
    dc->vmsd = &vmstate_ttsp;
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
    k->event = ttsp_event;
    k->send = ttsp_send;
    k->recv = ttsp_recv;
    k->match_and_add = ttsp_match;
}

static void tma460_init(Object *obj)
{
    CypressTTSPState *s = CYPRESS_TTSP(obj);

    s->gen4 = true;
    I2C_SLAVE(obj)->address = 0x24;
}

static void tma616_init(Object *obj)
{
    CypressTTSPState *s = CYPRESS_TTSP(obj);

    s->bl_addr = 0x69;
    I2C_SLAVE(obj)->address = 0x67;
}

static const TypeInfo ttsp_types[] = {
    {
        .name          = TYPE_CYPRESS_TTSP,
        .parent        = TYPE_I2C_SLAVE,
        .instance_size = sizeof(CypressTTSPState),
        .instance_init = ttsp_init,
        .class_init    = ttsp_class_init,
        .abstract      = true,
    }, {
        .name          = TYPE_CY_TMA460,
        .parent        = TYPE_CYPRESS_TTSP,
        .instance_init = tma460_init,
    }, {
        .name          = TYPE_CY_TMA616,
        .parent        = TYPE_CYPRESS_TTSP,
        .instance_init = tma616_init,
    },
};

DEFINE_TYPES(ttsp_types)
