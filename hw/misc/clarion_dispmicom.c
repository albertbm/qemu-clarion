/*
 * Display Micom — мікроконтролер панелі Clarion QY8XXX.
 *
 * На живій платі це окрема мікросхема в блоці дисплея, під'єднана до SCIF1
 * головного SoC. Це НЕ той самий вузол, що супутній МК плати на SCIF4
 * (hw/misc/clarion_micom.c): інший дріт, інший драйвер у гості і інший
 * формат тіла кадру. Без відповіді від панелі `lcddrv.dll` ніколи не
 * виставляє ACM-модуль 129, і DrawOi малює перший кадр аж після таймауту
 * очікування на 129.
 *
 * Протокол знято з коду lcddrv.dll, а не з документації.
 *
 * Обрамлення (приймач CLcdDrv::StartReadData @0xEF717578, передавач
 * CLcdDrv_Send @0xEF71A5D0):
 *
 *     10 02 | DATA... (стафінг: 10 -> 10 10) | 10 03 | BCC
 *     BCC = XOR усіх байтів ПІСЛЯ 10 02, разом із хвостовими 10 03,
 *     рахований по стафнутих байтах (eor/uxtb @0xEF7177B8, @0xEF71A65C)
 *
 * ⚠ На відміну від лінку плати, тут НЕМАЄ байта LEN: тіло починається
 * одразу з коду команди. Перевірка на справжньому кадрі гостя, знятому з
 * WriteFile: 10 02 23 10 03 30, XOR(23 10 03) = 0x30 ✔
 *
 * Квитанції — два байти поза кадром: DLE ACK = 10 06, DLE NAK = 10 15
 * (StartSendData_AckNack @0xEF71A4A0 кладе 0x10 у [this+0x552] і сам код у
 * [this+0x553], далі WriteFile на 2 байти). Приймач гостя впізнає їх у
 * StartReadData @0xEF7176D8.
 *
 * Автомат запуску (CLcdDrv::RcvAnalysisData @0xEF718554, диспетчер за
 * DATA[0]):
 *
 *     HU  -> 23                 запит інформації панелі (лінк-проба)
 *     HU  <- 10 06              квитанція
 *     HU  <- 24 VV UU P[12]     версія, UnitID, PartNum
 *                               обробник @0xEF7185B0 кладе DATA[1] у
 *                               [this+0x374], DATA[2] у [this+0x375],
 *                               12 байтів DATA[3..14] у [this+0x376],
 *                               далі Event(this, 1) -> стан лінка 0
 *                               -> CLcdDrv_Send::LinkStart @0xEF71A6B0:
 *                                  m_LinkState := 1 і черга на кадр 0x25
 *     HU  -> 25                 «лінк піднято»
 *     HU  <- 10 06              квитанція
 *     HU  <- 40                 обробник @0xEF718674: якщо m_LinkState == 1,
 *                               ставить 2 і кличе
 *                               CApi_ModuleInitializeComplete(129)
 *
 * Framing, ACKs and startup replies follow the guest lcddrv code. Ready
 * 0x40 reads no payload at ARM 0xef718674. Version/UnitID/PartNum are
 * SYNTHETIC: the ARM guest stores and prints them at 0xef718694,
 * 0xef7186a0 and 0xef7186ac..0xef7186d4 without comparing their values.
 *
 * Payload-free control completions are also supported: keepalive 0x71
 * (SH4 0x03cf6874, ARM 0xef718764), settings ready 0x42 (SH4 0x03cf677c)
 * and settings report completion 0x43 (SH4 0x03cf679e). Their receive
 * handlers read no response payload. Unsupported commands receive ACK.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/misc/clarion_dispmicom.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "standard-headers/linux/input-event-codes.h"
#include "ui/input.h"

#define DLE     0x10
#define STX     0x02
#define ETX     0x03
#define ACK     0x06
#define NAK     0x15

#define DISP_MAX_DATA       64
#define DISP_ACK_TIMEOUT_NS (500 * 1000 * 1000)
#define DISP_ACK_RETRIES    3

typedef struct DispFrame {
    GByteArray *bytes;
    bool wait_ack;
} DispFrame;

/* Коди, розібрані в lcddrv.dll. */
#define CMD_INFO_REQ        0x23    /* HU -> панель: запит інформації */
#define CMD_INFO_RSP        0x24    /* панель -> HU: версія/UnitID/PartNum */
#define CMD_LINK_UP         0x25    /* HU -> панель: лінк піднято */
#define CMD_READY           0x40    /* панель -> HU: готова; гість SET 129 */
#define CMD_SETTINGS        0x42
#define CMD_REPORT          0x43
#define CMD_KEEPALIVE       0x71
#define CMD_TOUCH           0x51
#define CMD_PANEL_KEY       0x34

/*
 * Пауза між кадрами — така сама, як у моделі МК плати: приймач SCIF має
 * FIFO на 16 байтів, і драйвер мусить устигнути забрати попередній кадр.
 * Час віртуальний, тож бут не залежить від завантаження хоста.
 */
#define DISP_GAP_NS         (20 * 1000 * 1000)
/* Доливання решти кадру, поки драйвер звільняє FIFO. */
#define DISP_FILL_NS        (1 * 1000 * 1000)

/*
 * СИНТЕТИЧНІ значення полів кадру 0x24. Гість їх не перевіряє (див. шапку),
 * а лише друкує. Обрано так, щоб у логу було видно, що це модель:
 *   Version 0x01, UnitID 0x01, PartNum "QEMUDISP001" (11 ASCII, як у
 *   форматі друку @0xEF7141F8), плюс дванадцятий байт, який гість копіює,
 *   але ніде не читає — 0x00.
 */
#define DISP_VERSION        0x01
#define DISP_UNIT_ID        0x01
static const char disp_partnum[11] = "QEMUDISP001";

typedef enum {
    RX_IDLE,            /* чекаємо DLE */
    RX_DLE,             /* бачили DLE поза кадром */
    RX_BODY,            /* усередині кадру */
    RX_BODY_DLE,        /* усередині кадру, бачили DLE */
    RX_BCC,             /* наступний байт — контрольна сума */
} DispRxState;

struct ClarionDispMicomState {
    DeviceState parent_obj;

    ClarionDispMicomSink sink;
    void *sink_opaque;

    QEMUTimer *tx;
    GQueue *frames;             /* queued panel frames */
    DispFrame *cur;             /* frame currently being sent */
    DispFrame *awaiting_ack;    /* sent key frame awaiting DLE ACK */
    unsigned cur_off;           /* скільки з нього вже прийнято */
    unsigned ack_retries;

    DispRxState rx_state;
    uint8_t body[DISP_MAX_DATA];
    unsigned body_len;
    uint8_t bcc;

    bool linked;                /* бачили 0x25 від гостя */
    bool touch_input;
    QemuInputHandlerState *input_handler;
    uint16_t touch_x;
    uint16_t touch_y;
    bool touch_down;
    bool touch_dirty;
    bool key_input;
    QemuInputHandlerState *key_input_handler;
    uint32_t key_down_mask;
};

static void disp_frame_free(DispFrame *frame)
{
    if (frame) {
        g_byte_array_free(frame->bytes, TRUE);
        g_free(frame);
    }
}

/* --- побудова кадру ---------------------------------------------------- */

static GByteArray *disp_build(const uint8_t *data, unsigned len)
{
    GByteArray *f = g_byte_array_new();
    uint8_t b, bcc = 0;
    unsigned i;

    b = DLE; g_byte_array_append(f, &b, 1);
    b = STX; g_byte_array_append(f, &b, 1);

    /* тіло = сам DATA, без LEN; стафінг DLE, BCC по стафнутих байтах */
    for (i = 0; i < len; i++) {
        uint8_t v = data[i];

        g_byte_array_append(f, &v, 1);
        bcc ^= v;
        if (v == DLE) {
            g_byte_array_append(f, &v, 1);
            bcc ^= v;
        }
    }
    b = DLE; g_byte_array_append(f, &b, 1); bcc ^= b;
    b = ETX; g_byte_array_append(f, &b, 1); bcc ^= b;
    g_byte_array_append(f, &bcc, 1);
    return f;
}

static void disp_arm(ClarionDispMicomState *s, int64_t delay)
{
    timer_mod(s->tx, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + delay);
}

static void disp_trace(const char *direction, const uint8_t *data,
                       unsigned len)
{
    unsigned i;
    g_autoptr(GString) body = g_string_new(NULL);

    for (i = 0; i < len; i++) {
        g_string_append_printf(body, "%s%02x", i ? " " : "", data[i]);
    }
    qemu_log("clarion-dispmicom: trace virtual-ms=%" PRId64
             " dir=%s body=%s\n", qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL),
             direction, body->str);
}

static void disp_queue(ClarionDispMicomState *s, const uint8_t *data,
                       unsigned len)
{
    DispFrame *frame = g_new0(DispFrame, 1);

    frame->bytes = disp_build(data, len);
    g_queue_push_tail(s->frames, frame);
    if (!s->cur && !s->awaiting_ack) {
        disp_arm(s, DISP_FILL_NS);
    }
}

static void disp_queue_key(ClarionDispMicomState *s, const uint8_t *data,
                           unsigned len)
{
    DispFrame *frame = g_new0(DispFrame, 1);

    frame->bytes = disp_build(data, len);
    frame->wait_ack = true;
    g_queue_push_tail(s->frames, frame);
    if (!s->cur && !s->awaiting_ack) {
        disp_arm(s, DISP_FILL_NS);
    }
}

static void disp_queue_ack(ClarionDispMicomState *s, uint8_t code)
{
    DispFrame *frame = g_new0(DispFrame, 1);
    uint8_t b = DLE;

    frame->bytes = g_byte_array_new();
    g_byte_array_append(frame->bytes, &b, 1);
    g_byte_array_append(frame->bytes, &code, 1);
    g_queue_push_head(s->frames, frame);
    if (!s->cur && !s->awaiting_ack) {
        disp_arm(s, DISP_FILL_NS);
    }
}

/* --- віддавання байтів у SCIF ------------------------------------------ */

static void disp_tx_expire(void *opaque)
{
    ClarionDispMicomState *s = opaque;
    int took;

    if (!s->cur) {
        if (s->awaiting_ack) {
            if (s->ack_retries >= DISP_ACK_RETRIES) {
                qemu_log("clarion-dispmicom: key-ack-timeout retries=%u\n",
                         s->ack_retries);
                disp_frame_free(s->awaiting_ack);
                s->awaiting_ack = NULL;
                s->ack_retries = 0;
                if (!g_queue_is_empty(s->frames)) {
                    disp_arm(s, DISP_GAP_NS);
                }
                return;
            }
            s->cur = s->awaiting_ack;
            s->awaiting_ack = NULL;
            s->cur_off = 0;
            s->ack_retries++;
            qemu_log("clarion-dispmicom: key-ack-retry attempt=%u\n",
                     s->ack_retries);
        } else {
            s->cur = g_queue_pop_head(s->frames);
        }
        s->cur_off = 0;
        if (!s->cur) {
            return;
        }
    }
    if (!s->sink) {
        return;
    }
    took = s->sink(s->sink_opaque, s->cur->bytes->data + s->cur_off,
                   s->cur->bytes->len - s->cur_off);
    if (took > 0) {
        if (!s->cur_off) {
            if (s->cur->bytes->len == 2) {
                disp_trace("panel-tx", s->cur->bytes->data, 2);
            } else {
                uint8_t data[DISP_MAX_DATA];
                unsigned len = 0;
                unsigned i;

                for (i = 2; i < s->cur->bytes->len - 3; i++) {
                    data[len++] = s->cur->bytes->data[i];
                    if (s->cur->bytes->data[i] == DLE) {
                        i++;
                    }
                }
                disp_trace("panel-tx", data, len);
            }
        }
        s->cur_off += took;
    }
    if (s->cur_off >= s->cur->bytes->len) {
        if (s->cur->wait_ack) {
            s->awaiting_ack = s->cur;
            s->cur = NULL;
            s->cur_off = 0;
            disp_arm(s, DISP_ACK_TIMEOUT_NS);
            return;
        }
        disp_frame_free(s->cur);
        s->cur = NULL;
        s->cur_off = 0;
        if (!g_queue_is_empty(s->frames)) {
            disp_arm(s, DISP_GAP_NS);       /* пауза між кадрами */
        }
    } else {
        disp_arm(s, DISP_FILL_NS);          /* FIFO повний — доллємо згодом */
    }
}

static void disp_guest_ack(ClarionDispMicomState *s, uint8_t code)
{
    if (!s->awaiting_ack) {
        return;
    }
    timer_del(s->tx);
    if (code == ACK) {
        qemu_log("clarion-dispmicom: key-ack accepted retries=%u\n",
                 s->ack_retries);
        disp_frame_free(s->awaiting_ack);
        s->awaiting_ack = NULL;
        s->ack_retries = 0;
        if (!g_queue_is_empty(s->frames)) {
            disp_arm(s, DISP_GAP_NS);
        }
    } else {
        qemu_log("clarion-dispmicom: key-nak retries=%u\n", s->ack_retries);
        if (s->ack_retries >= DISP_ACK_RETRIES) {
            disp_frame_free(s->awaiting_ack);
            s->awaiting_ack = NULL;
            s->ack_retries = 0;
            if (!g_queue_is_empty(s->frames)) {
                disp_arm(s, DISP_GAP_NS);
            }
        } else {
            s->cur = s->awaiting_ack;
            s->awaiting_ack = NULL;
            s->cur_off = 0;
            s->ack_retries++;
            disp_arm(s, DISP_FILL_NS);
        }
    }
}

/* --- обробка кадру від гостя ------------------------------------------- */

static void disp_send_info(ClarionDispMicomState *s)
{
    uint8_t data[3 + 12];

    data[0] = CMD_INFO_RSP;
    data[1] = DISP_VERSION;
    data[2] = DISP_UNIT_ID;
    memcpy(&data[3], disp_partnum, sizeof(disp_partnum));
    data[3 + sizeof(disp_partnum)] = 0x00;  /* гість копіює, але не читає */
    disp_queue(s, data, sizeof(data));
}

static void disp_frame(ClarionDispMicomState *s)
{
    uint8_t cmd;

    disp_trace("guest-tx", s->body, s->body_len);

    /* На кожен кадр — DLE ACK, як у живому лінку. */
    disp_queue_ack(s, ACK);

    if (!s->body_len) {
        return;
    }
    cmd = s->body[0];

    switch (cmd) {
    case CMD_INFO_REQ:
        s->linked = false;
        disp_send_info(s);
        break;

    case CMD_LINK_UP:
        s->linked = true;
        disp_queue(s, (const uint8_t[]){ CMD_READY }, 1);
        break;

    case CMD_KEEPALIVE:
        /*
         * Keepalive completion: SH4 handler 0x03cf6874 and ARM handler
         * 0xef718764 clear the outstanding flag and cancel timer 2.
         * Neither handler reads a payload byte.
         */
        disp_queue(s, (const uint8_t[]){ CMD_KEEPALIVE }, 1);
        break;

    case CMD_SETTINGS:
        /*
         * SH4 handler 0x03cf677c accepts ready 0x42 in link state 1,
         * reads no payload, and queues the 0x43 settings report.
         */
        disp_queue(s, (const uint8_t[]){ CMD_SETTINGS }, 1);
        break;

    case CMD_REPORT:
        /*
         * SH4 handler 0x03cf679e changes link state 1 to 2. It logs
         * the previously saved panel identity, not a response payload.
         */
        disp_queue(s, (const uint8_t[]){ CMD_REPORT }, 1);
        break;

    default:
        /* Решта команд панелі не розібрана — лише квитанція, без вигадок. */
        break;
    }
}

/* --- synthetic QY7 touch input ---------------------------------------- */

static void disp_touch_event(DeviceState *dev, QemuConsole *src,
                             QemuInputEvent *evt)
{
    ClarionDispMicomState *s = CLARION_DISPMICOM(dev);

    (void)src;

    switch (evt->type) {
    case INPUT_EVENT_KIND_ABS:
        if (evt->abs.axis == INPUT_AXIS_X) {
            s->touch_x =
                qemu_input_scale_axis(evt->abs.value, INPUT_EVENT_ABS_MIN,
                                      INPUT_EVENT_ABS_MAX, 0, 800);
            s->touch_dirty = true;
        } else if (evt->abs.axis == INPUT_AXIS_Y) {
            s->touch_y = 480 -
                qemu_input_scale_axis(evt->abs.value, INPUT_EVENT_ABS_MIN,
                                      INPUT_EVENT_ABS_MAX, 0, 479);
            s->touch_dirty = true;
        }
        break;
    case INPUT_EVENT_KIND_BTN:
        if (evt->btn.button == INPUT_BUTTON_LEFT) {
            s->touch_down = evt->btn.down;
            s->touch_dirty = true;
        }
        break;
    default:
        break;
    }
}

static void disp_touch_sync(DeviceState *dev)
{
    ClarionDispMicomState *s = CLARION_DISPMICOM(dev);
    uint8_t data[] = {
        CMD_TOUCH,         s->touch_x >> 8, s->touch_x & 0xff,
        s->touch_y >> 8, s->touch_y & 0xff,   s->touch_down ? '1' : '0',
    };

    if (!s->touch_dirty) {
        return;
    }
    s->touch_dirty = false;
    qemu_log("clarion-dispmicom: synthetic-touch virtual-ms=%" PRId64
             " raw=%u,%u down=%u\n",
             qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL), s->touch_x, s->touch_y,
             s->touch_down);
    disp_trace("synthetic-touch", data + 1, sizeof(data) - 1);
    disp_queue(s, data, sizeof(data));
}

static const QemuInputHandler disp_touch_handler = {
    .name = "qy7-touch",
    .mask = INPUT_EVENT_MASK_ABS | INPUT_EVENT_MASK_BTN,
    .event = disp_touch_event,
    .sync = disp_touch_sync,
};

typedef struct DispKeyMap {
    unsigned int qemu_key;
    uint8_t panel_code;
} DispKeyMap;

static const DispKeyMap disp_key_map[] = {
    { KEY_M, 1 },
    { KEY_B, 0x21 },
};

static void disp_key_event(DeviceState *dev, QemuConsole *src,
                           QemuInputEvent *evt)
{
    ClarionDispMicomState *s = CLARION_DISPMICOM(dev);
    uint8_t data[2] = { CMD_PANEL_KEY, 0 };
    unsigned int i;

    (void)src;
    if (evt->type != INPUT_EVENT_KIND_KEY) {
        return;
    }
    for (i = 0; i < ARRAY_SIZE(disp_key_map); i++) {
        if (evt->key.key == disp_key_map[i].qemu_key) {
            uint32_t bit = 1U << i;

            if (evt->key.down) {
                if (s->key_down_mask & bit) {
                    qemu_log("clarion-dispmicom: panel-key repeat-suppressed "
                             "code=%02x\n", disp_key_map[i].panel_code);
                    return;
                }
                s->key_down_mask |= bit;
            } else {
                if (!(s->key_down_mask & bit)) {
                    return;
                }
                s->key_down_mask &= ~bit;
            }
            data[1] = disp_key_map[i].panel_code |
                      (evt->key.down ? 0x80 : 0x00);
            qemu_log("clarion-dispmicom: panel-key code=%02x down=%u\n",
                     disp_key_map[i].panel_code, evt->key.down);
            disp_queue_key(s, data, sizeof(data));
            return;
        }
    }
}

static const QemuInputHandler disp_key_handler = {
    .name = "qy7-panel-keys",
    .mask = INPUT_EVENT_MASK_KEY,
    .event = disp_key_event,
};

/* --- розбір потоку від гостя ------------------------------------------- */

void clarion_dispmicom_rx_byte(DeviceState *dev, uint8_t b)
{
    ClarionDispMicomState *s = CLARION_DISPMICOM(dev);

    switch (s->rx_state) {
    case RX_IDLE:
        if (b == DLE) {
            s->rx_state = RX_DLE;
        }
        break;

    case RX_DLE:
        if (b == STX) {
            s->body_len = 0;
            s->bcc = 0;
            s->rx_state = RX_BODY;
        } else if (b == DLE) {
            s->rx_state = RX_DLE;           /* 10 10 поза кадром — тримаємось */
        } else {
            if (b == ACK || b == NAK) {
                disp_trace("guest-tx", (const uint8_t[]){ DLE, b }, 2);
                disp_guest_ack(s, b);
            }
            /* DLE ACK / DLE NAK від гостя — підтвердження наших кадрів */
            s->rx_state = RX_IDLE;
        }
        break;

    case RX_BODY:
        s->bcc ^= b;
        if (b == DLE) {
            s->rx_state = RX_BODY_DLE;
        } else if (s->body_len < sizeof(s->body)) {
            s->body[s->body_len++] = b;
        }
        break;

    case RX_BODY_DLE:
        s->bcc ^= b;
        if (b == DLE) {                     /* стафінг: один літеральний 0x10 */
            if (s->body_len < sizeof(s->body)) {
                s->body[s->body_len++] = DLE;
            }
            s->rx_state = RX_BODY;
        } else if (b == ETX) {
            s->rx_state = RX_BCC;
        } else {
            s->rx_state = RX_IDLE;          /* поламаний кадр — кидаємо */
        }
        break;

    case RX_BCC:
        if (b != s->bcc) {
            qemu_log_mask(LOG_GUEST_ERROR, "clarion-dispmicom: BCC %#x, чекали "
                          "%#x — кадр відкинуто\n", b, s->bcc);
            disp_queue_ack(s, NAK);
        } else {
            disp_frame(s);
        }
        s->rx_state = RX_IDLE;
        break;
    }
}

static void disp_set_key_raw(Object *obj, const char *value, Error **errp)
{
    ClarionDispMicomState *s = CLARION_DISPMICOM(obj);
    uint8_t data[DISP_MAX_DATA];
    const char *p = value;
    unsigned len = 0;

    if (!s->key_input) {
        error_setg(errp, "raw panel keys are enabled only on QY7");
        return;
    }
    while (*p) {
        char *end;
        unsigned long byte;

        while (g_ascii_isspace(*p) || *p == ',' || *p == ':') {
            p++;
        }
        if (!*p) {
            break;
        }
        if (!g_ascii_isxdigit(*p) || len == DISP_MAX_DATA) {
            error_setg(errp, "key-raw expects 1 to %u hex bytes",
                       DISP_MAX_DATA);
            return;
        }
        errno = 0;
        byte = g_ascii_strtoull(p, &end, 16);
        if (errno || end == p || byte > 0xff ||
            (*end && !g_ascii_isspace(*end) && *end != ',' && *end != ':')) {
            error_setg(errp, "invalid key-raw byte near '%s'", p);
            return;
        }
        data[len++] = byte;
        p = end;
    }
    if (!len) {
        error_setg(errp, "key-raw requires at least a command byte");
        return;
    }
    disp_trace("key-raw", data, len);
    disp_queue_key(s, data, len);
}

void clarion_dispmicom_set_sink(DeviceState *dev, ClarionDispMicomSink fn,
                                void *opaque)
{
    ClarionDispMicomState *s = CLARION_DISPMICOM(dev);

    s->sink = fn;
    s->sink_opaque = opaque;
}

/* --- QOM --------------------------------------------------------------- */

static void clarion_dispmicom_reset_hold(Object *obj, ResetType type)
{
    ClarionDispMicomState *s = CLARION_DISPMICOM(obj);

    while (!g_queue_is_empty(s->frames)) {
        disp_frame_free(g_queue_pop_head(s->frames));
    }
    if (s->cur) {
        disp_frame_free(s->cur);
        s->cur = NULL;
    }
    disp_frame_free(s->awaiting_ack);
    s->awaiting_ack = NULL;
    s->cur_off = 0;
    s->ack_retries = 0;
    s->rx_state = RX_IDLE;
    s->body_len = 0;
    s->bcc = 0;
    s->linked = false;
    s->key_down_mask = 0;
}

static void clarion_dispmicom_realize(DeviceState *dev, Error **errp)
{
    ClarionDispMicomState *s = CLARION_DISPMICOM(dev);

    s->frames = g_queue_new();
    s->tx = timer_new_ns(QEMU_CLOCK_VIRTUAL, disp_tx_expire, s);
    if (s->key_input) {
        s->key_input_handler =
            qemu_input_handler_register(dev, &disp_key_handler);
        qemu_input_handler_activate(s->key_input_handler);
        object_property_add_str(OBJECT(s), "key-raw", NULL,
                                disp_set_key_raw);
    }
    if (s->touch_input) {
        s->input_handler =
            qemu_input_handler_register(dev, &disp_touch_handler);
        qemu_input_handler_activate(s->input_handler);
    }
}

static const Property clarion_dispmicom_props[] = {
    DEFINE_PROP_BOOL("touch-input", ClarionDispMicomState, touch_input, false),
    DEFINE_PROP_BOOL("key-input", ClarionDispMicomState, key_input, false),
};

static void clarion_dispmicom_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = clarion_dispmicom_realize;
    dc->desc = "Clarion display panel micom (QY7 SCIF3 or QY8 SCIF1)";
    device_class_set_props(dc, clarion_dispmicom_props);
    rc->phases.hold = clarion_dispmicom_reset_hold;
}

static const TypeInfo clarion_dispmicom_types[] = {
    {
        .name = TYPE_CLARION_DISPMICOM,
        .parent = TYPE_DEVICE,
        .instance_size = sizeof(ClarionDispMicomState),
        .class_init = clarion_dispmicom_class_init,
    },
};

DEFINE_TYPES(clarion_dispmicom_types)
