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
 * ⚠ Межа чесності. Тут відтворено рівно те, що доведене кодом lcddrv.dll:
 * обрамлення, стафінг, BCC, квитанції і ці три коди команд. Обробник 0x40
 * НЕ читає з кадру жодного байта (див. @0xEF718674), тому кадр віддається
 * без корисного навантаження. Значення Version/UnitID/PartNum синтетичні:
 * гість їх тільки зберігає й друкує у лог (єдині читачі — @0xEF718694,
 * @0xEF7186A0, @0xEF7186AC..@0xEF7186D4), ніде не порівнює, тож будь-які
 * семантично коректні байти безпечні. Інші коди команд панелі свідомо НЕ
 * вигадуються: на них модель відповідає лише квитанцією.
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
#include "ui/input.h"

#define DLE     0x10
#define STX     0x02
#define ETX     0x03
#define ACK     0x06
#define NAK     0x15

#define DISP_MAX_DATA       64

/* Коди, розібрані в lcddrv.dll. */
#define CMD_INFO_REQ        0x23    /* HU -> панель: запит інформації */
#define CMD_INFO_RSP        0x24    /* панель -> HU: версія/UnitID/PartNum */
#define CMD_LINK_UP         0x25    /* HU -> панель: лінк піднято */
#define CMD_READY           0x40    /* панель -> HU: готова; гість SET 129 */
/* CLcdDrv_Send @0xEF71A7F4 ставить [+0x33d]; лише відповідь 0x71 його знімає */
#define CMD_POLL            0x71
/* kepdrv InputLcdKey (QY8605NB @0xEF880F1C): DATA[1] = матричний код, біт 7 = натиснуто */
#define CMD_KEY             0x34
#define KEY_PRESSED         0x80

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
    GQueue *frames;             /* черга GByteArray * — готові кадри */
    GByteArray *cur;            /* кадр, який зараз віддаємо */
    unsigned cur_off;           /* скільки з нього вже прийнято */

    DispRxState rx_state;
    uint8_t body[DISP_MAX_DATA];
    unsigned body_len;
    uint8_t bcc;

    bool linked;                /* бачили 0x25 від гостя */

    QEMUTimer *key_up;          /* відпускання клавіші, вставленої з монітора */
    uint8_t key_held;

    QemuInputHandlerState *ih;  /* host keyboard, parallel to the qom-set path */
};

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

static void disp_queue(ClarionDispMicomState *s, const uint8_t *data,
                       unsigned len)
{
    g_queue_push_tail(s->frames, disp_build(data, len));
    if (!s->cur) {
        disp_arm(s, DISP_FILL_NS);
    }
}

static void disp_queue_ack(ClarionDispMicomState *s, uint8_t code)
{
    GByteArray *f = g_byte_array_new();
    uint8_t b = DLE;

    g_byte_array_append(f, &b, 1);
    g_byte_array_append(f, &code, 1);
    g_queue_push_head(s->frames, f);        /* квитанція йде поперед черги */
    if (!s->cur) {
        disp_arm(s, DISP_FILL_NS);
    }
}

/* --- віддавання байтів у SCIF ------------------------------------------ */

static void disp_tx_expire(void *opaque)
{
    ClarionDispMicomState *s = opaque;
    int took;

    if (!s->cur) {
        s->cur = g_queue_pop_head(s->frames);
        s->cur_off = 0;
        if (!s->cur) {
            return;
        }
    }
    if (!s->sink) {
        return;
    }
    took = s->sink(s->sink_opaque, s->cur->data + s->cur_off,
                   s->cur->len - s->cur_off);
    if (took > 0) {
        s->cur_off += took;
    }
    if (s->cur_off >= s->cur->len) {
        g_byte_array_free(s->cur, TRUE);
        s->cur = NULL;
        s->cur_off = 0;
        if (!g_queue_is_empty(s->frames)) {
            disp_arm(s, DISP_GAP_NS);       /* пауза між кадрами */
        }
    } else {
        disp_arm(s, DISP_FILL_NS);          /* FIFO повний — доллємо згодом */
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

    case CMD_POLL:
        disp_queue(s, (const uint8_t[]){ CMD_POLL }, 1);
        break;

    default:
        /* Решта команд панелі не розібрана — лише квитанція, без вигадок. */
        break;
    }
}

/* --- розбір потоку від гостя ------------------------------------------- */

static void disp_key_up(void *opaque)
{
    ClarionDispMicomState *s = opaque;

    disp_queue(s, (const uint8_t[]){ CMD_KEY, s->key_held }, 2);
}

/* qom-set key="CODE[,HOLD_MS]": натискання панельної клавіші за матричним кодом */
static void disp_set_key(Object *obj, const char *value, Error **errp)
{
    ClarionDispMicomState *s = CLARION_DISPMICOM(obj);
    unsigned long code, hold = 150;
    char *end;

    code = strtoul(value, &end, 16);
    if (end != value && *end == ',') {
        hold = strtoul(end + 1, &end, 10);
    }
    if (end == value || code > 0x7f || *end) {
        error_setg(errp, "key: expected CODE[,HOLD_MS], CODE hex 00..7f");
        return;
    }
    if (timer_pending(s->key_up)) {
        timer_del(s->key_up);
        disp_key_up(s);
    }
    s->key_held = code;
    disp_queue(s, (const uint8_t[]){ CMD_KEY, code | KEY_PRESSED }, 2);
    timer_mod(s->key_up, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              (int64_t)hold * 1000000);
}

/* qom-set frame="34 05": сирий кадр панелі для дослідів з іншими командами */
static void disp_set_frame(Object *obj, const char *value, Error **errp)
{
    ClarionDispMicomState *s = CLARION_DISPMICOM(obj);
    uint8_t data[DISP_MAX_DATA];
    unsigned len = 0;
    const char *p = value;
    char *end;

    while (*p) {
        unsigned long v = strtoul(p, &end, 16);

        if (end == p || v > 0xff || len == sizeof(data)) {
            error_setg(errp, "frame: expected hex bytes");
            return;
        }
        data[len++] = v;
        p = end + strspn(end, " ,:");
    }
    if (len) {
        disp_queue(s, data, len);
    }
}

/* bezel codes per InputLcdKey (kepdrv, QY8605NB @0xEF880F1C); F5-F9 unproven */
static int disp_key_code(int qcode)
{
    switch (qcode) {
    case Q_KEY_CODE_F1: return 0x01;    /* Menu */
    case Q_KEY_CODE_F2: return 0x03;    /* Back */
    case Q_KEY_CODE_F3: return 0x31;    /* Screen off, real unit wants a 2s hold */
    case Q_KEY_CODE_F4: return 0x32;    /* Current position */
    case Q_KEY_CODE_F5: return 0x13;    /* Camera? */
    case Q_KEY_CODE_F6: return 0x30;    /* Audio-TV? may be swapped with F5 */
    case Q_KEY_CODE_F7: return 0x00;    /* seek pair, A side, direction unproven */
    case Q_KEY_CODE_F8: return 0x20;    /* seek pair, B side */
    case Q_KEY_CODE_F9: return 0x10;    /* TUNE knob push */
    default: return -1;
    }
}

/* host keyboard, separate from the qom-set path above but the same wire frame */
static void disp_key_event(DeviceState *dev, QemuConsole *src,
                           QemuInputEvent *evt)
{
    ClarionDispMicomState *s = CLARION_DISPMICOM(dev);
    int code = disp_key_code(qemu_input_linux_to_qcode(evt->key.key));

    if (code < 0) {
        return;
    }
    disp_queue(s, (const uint8_t[]){ CMD_KEY,
               code | (evt->key.down ? KEY_PRESSED : 0) }, 2);
}

static const QemuInputHandler disp_key_handler = {
    .name  = "Clarion QY8 panel keys",
    .mask  = INPUT_EVENT_MASK_KEY,
    .event = disp_key_event,
};

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
        g_byte_array_free(g_queue_pop_head(s->frames), TRUE);
    }
    if (s->cur) {
        g_byte_array_free(s->cur, TRUE);
        s->cur = NULL;
    }
    s->cur_off = 0;
    s->rx_state = RX_IDLE;
    s->body_len = 0;
    s->bcc = 0;
    s->linked = false;
    timer_del(s->key_up);
}

static void clarion_dispmicom_realize(DeviceState *dev, Error **errp)
{
    ClarionDispMicomState *s = CLARION_DISPMICOM(dev);

    s->frames = g_queue_new();
    s->tx = timer_new_ns(QEMU_CLOCK_VIRTUAL, disp_tx_expire, s);
    s->key_up = timer_new_ns(QEMU_CLOCK_VIRTUAL, disp_key_up, s);
    s->ih = qemu_input_handler_register(dev, &disp_key_handler);
}

static void clarion_dispmicom_unrealize(DeviceState *dev)
{
    ClarionDispMicomState *s = CLARION_DISPMICOM(dev);

    qemu_input_handler_unregister(s->ih);
}

static void clarion_dispmicom_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = clarion_dispmicom_realize;
    dc->unrealize = clarion_dispmicom_unrealize;
    dc->desc = "Clarion QY8XXX display panel micom (SCIF1 link)";
    rc->phases.hold = clarion_dispmicom_reset_hold;
    object_class_property_add_str(klass, "key", NULL, disp_set_key);
    object_class_property_add_str(klass, "frame", NULL, disp_set_frame);
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
