/*
 * Renesas CAN controller (R-Car Gen1, R8A7778) — QY8XXX @ 0xFFFD1000.
 *
 * Це КОНТРОЛЕР, а не автомобіль. Модель знає лише про власні режими
 * (reset / halt / operation / sleep) і про пам'ять поштових скриньок.
 * Жодного CAN-буса тут немає: ні передачі, ні приймання поки не
 * реалізовано, і модель нічого не вигадує замість них.
 *
 * --- Звідки взята мапа регістрів ------------------------------------------
 *
 * 1. Жива траса QEMU (docs/04-journal.md, 2026-09-24): 64 групи по 16 байтів
 *    за +0x000, 64 однобайтові регістри за +0x800, далі окремі регістри
 *    +0x840, +0x842, +0x844/846, +0x84C, +0x856, +0x858.
 * 2. Драйвер `CAN.dll` (секція 0, VA 0xEF621000) — саме з нього знято
 *    семантику, а не з даташита (офіційного опису цього блока у відкритих
 *    джерелах немає).
 * 3. Імена регістрів — з `struct rcar_can_regs` драйвера Linux
 *    `drivers/net/can/rcar/rcar_can.c`, чия мапа збігається з (1) байт у
 *    байт: mb[64]×16, mkr/fidcr/mier за +0x400, mctl[64] за +0x800,
 *    CTLR +0x840, STR +0x842, BCR +0x844..846.
 *
 * --- Доведені точки драйвера ----------------------------------------------
 *
 *   0xEF628E80  вихід зі сну: якщо CTLR & 0x400 — зняти біт і чекати,
 *               поки згасне STR & 0x400.
 *   0xEF629308  ініціалізація:
 *                 CTLR = (CTLR & ~0x300) | 0x100      — запит reset mode
 *                 100 × опитування STR & 0x100        — інакше помилка 0x21
 *                 очищення 64 скриньок, MCTL[i] = 0x40
 *                 BCR: 0xF004 -> +0x844, 0x12 -> +0x846 (біт-тайминг)
 *                 CTLR = (CTLR & ~0x300) | 0x200      — запит halt mode
 *                 100 × опитування STR & 0x200        — інакше помилка 0x21
 *                 +0x858 = (+0x858 & 0x20) | 0x07, +0x84C = 0x08
 *                 CTLR &= ~0x300                      — запит operation mode
 *                 100 × очікування, поки згаснуть ОБИДВА STR 0x100 і 0x200
 *   0xEF6294BC  самоперевірка пам'яті скриньок: у кожну з 64 пише 0x5555 і
 *               0xAAAA за зміщеннями 0x0, 0x2, 0x6, 0x8, 0xA, 0xC і звіряє
 *               читання з маскою (таблиця за 0xEF6216AC). Маска першого
 *               півслова — 0xDFFF, решти — 0xFFFF.
 *
 * --- Межа чесності --------------------------------------------------------
 *
 * * Скидальне значення CTLR даташитом не підтверджене. Обрано 0x0100 —
 *   контролер після ввімкнення живлення стоїть у reset mode, як і будь-який
 *   CAN-контролер. Драйвер до цього нечутливий: він сам примусово задає
 *   CANM, перш ніж чекати на підтвердження.
 * * Перехід у operation mode на справжньому залізі стається після 11
 *   рецесивних бітів на шині. Приймача до моделі не під'єднано, шина вільна,
 *   тобто 11 рецесивних бітів минають одразу — тому перехід миттєвий. Це
 *   фізика вільної шини, а не заглушка.
 * * Лічильники помилок (RECR/TECR/ECSR) читаються нулем: без шини помилок
 *   немає. MSSR віддає SEST=1 — «жодної скриньки не знайдено», бо приймати
 *   нема чого.
 * * Біт 13 першого півслова скриньки читається нулем — це єдиний біт, який
 *   сам драйвер виключає зі своєї самоперевірки (маска 0xDFFF). У мапі
 *   Linux цьому відповідає резервний біт 29 32-бітного поля ID.
 * * Переривання не виводиться взагалі: жодна подія моделі його не піднімає,
 *   а номер лінії GIC для CAN нічим не доведено.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "accel/tcg/cpu-loop.h"
#include "exec/target_page.h"
#include "hw/core/cpu.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"
#include "hw/net/renesas_can.h"
#include "migration/vmstate.h"
#include "qom/object.h"
#include "trace.h"

OBJECT_DECLARE_SIMPLE_TYPE(RenesasCANState, RENESAS_CAN)

/* --- мапа блока -------------------------------------------------------- */

#define CAN_MB_BASE     0x000   /* 64 скриньки по 16 байтів               */
#define CAN_MB_SIZE     0x400
#define CAN_MKR_BASE    0x400   /* MKR2..9, FIDCR0/1, MKIVLR*, MIER*      */
#define CAN_MKR_SIZE    0x040
#define CAN_CTL_BASE    0x800   /* MCTL[64] і далі керівні регістри       */
#define CAN_CTL_SIZE    0x100

#define CAN_CTLR        0x840   /* Control Register,  16 біт             */
#define CAN_STR         0x842   /* Status Register,   16 біт, лише R     */

/* CTLR */
#define CTLR_CANM       0x0300  /* режим контролера                      */
#define CTLR_CANM_OPER  0x0000
#define CTLR_CANM_RESET 0x0100
#define CTLR_CANM_HALT  0x0200
#define CTLR_CANM_FRST  0x0300  /* force reset                           */
#define CTLR_SLPM       0x0400  /* запит сну                             */

/* STR */
#define STR_RSTST       0x0100
#define STR_HLTST       0x0200
#define STR_SLPST       0x0400

/* Регістри, що мають не просто пам'ять (зміщення в межах CAN_CTL_BASE) */
#define CTL_OFF(x)      ((x) - CAN_CTL_BASE)
#define CTL_MSSR        CTL_OFF(0x852)
#define MSSR_SEST       0x80    /* 1 = результату пошуку немає           */

enum {
    CAN_MODE_RESET,
    CAN_MODE_HALT,
    CAN_MODE_OPERATION,
};

struct RenesasCANState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;

    uint8_t mb[CAN_MB_SIZE];    /* пам'ять поштових скриньок             */
    uint8_t mkr[CAN_MKR_SIZE];  /* маски/фільтри/дозволи переривань      */
    uint8_t ctl[CAN_CTL_SIZE];  /* MCTL[64] + керівні регістри           */

    uint8_t mode;               /* CAN_MODE_*                            */
    bool sleep;

    /* QY8_CAN_PC=1 — дописувати до кожного рядка траси pc/lr викликача */
    bool trace_pc;
};

static const char *can_mode_name(uint8_t mode)
{
    switch (mode) {
    case CAN_MODE_RESET:     return "reset";
    case CAN_MODE_HALT:      return "halt";
    case CAN_MODE_OPERATION: return "operation";
    default:                 return "?";
    }
}

/*
 * STR не зберігається, а виводиться зі стану контролера — інакше це була б
 * та сама підробка «читання повертає очікуване».
 */
static uint16_t can_str(RenesasCANState *s)
{
    uint16_t str = 0;

    switch (s->mode) {
    case CAN_MODE_RESET:
        str |= STR_RSTST;
        break;
    case CAN_MODE_HALT:
        str |= STR_HLTST;
        break;
    default:
        break;
    }
    if (s->sleep) {
        str |= STR_SLPST;
    }
    return str;
}

static void can_ctlr_write(RenesasCANState *s, uint16_t ctlr)
{
    uint8_t old = s->mode;
    bool oldsleep = s->sleep;

    switch (ctlr & CTLR_CANM) {
    case CTLR_CANM_RESET:
    case CTLR_CANM_FRST:
        s->mode = CAN_MODE_RESET;
        break;
    case CTLR_CANM_HALT:
        s->mode = CAN_MODE_HALT;
        break;
    default:
        /*
         * Запит робочого режиму. На залізі контролер приєднується до шини,
         * побачивши 11 рецесивних бітів; шина вільна, тож це відбувається
         * одразу.
         */
        s->mode = CAN_MODE_OPERATION;
        break;
    }
    s->sleep = (ctlr & CTLR_SLPM) != 0;

    if (s->mode != old || s->sleep != oldsleep) {
        trace_renesas_can_mode(can_mode_name(old), can_mode_name(s->mode),
                               s->sleep, can_str(s));
    }
}

/* --- доступ до байтових масивів будь-якою шириною ---------------------- */

static uint64_t can_ld(const uint8_t *p, unsigned size)
{
    uint64_t v = 0;
    unsigned i;

    for (i = 0; i < size; i++) {
        v |= (uint64_t)p[i] << (8 * i);
    }
    return v;
}

static void can_st(uint8_t *p, unsigned size, uint64_t v)
{
    unsigned i;

    for (i = 0; i < size; i++) {
        p[i] = v >> (8 * i);
    }
}

/*
 * Хто саме звертається. Той самий прийом, що й у qy8.periph: точний PC
 * інструкції дає cpu_unwind_state_data (НЕ cpu_restore_state — той пише
 * розгорнутий стан назад у env і гість іде іншою гілкою). Рахується лише
 * при QY8_CAN_PC=1, бо розгортання стану дороге.
 */
static void can_ctx(RenesasCANState *s, char *buf, size_t len)
{
    CPUState *cs = current_cpu;
    uint64_t data[4];
    uint64_t pc;

    buf[0] = '\0';
    if (!s->trace_pc || !cs) {
        return;
    }
    if (cpu_unwind_state_data(cs, cs->mem_io_pc, data)) {
        /*
         * З CF_PCREL data[0] — зсув усередині сторінки; старші біти беремо
         * з поточного PC, бо блок трансляції не перетинає межі сторінки.
         */
        pc = data[0];
        if (pc < qemu_target_page_size()) {
            pc |= CPU_GET_CLASS(cs)->get_pc(cs) & qemu_target_page_mask();
        }
    } else {
        pc = CPU_GET_CLASS(cs)->get_pc(cs);   /* запасний: початок блока */
    }
    snprintf(buf, len, " pc=0x%08" PRIx64 " t=%" PRId64, pc,
             qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL));
}

static uint64_t can_read(void *opaque, hwaddr addr, unsigned size)
{
    RenesasCANState *s = opaque;
    char ctx[80];
    uint64_t val;

    if (addr < CAN_MB_BASE + CAN_MB_SIZE) {
        val = can_ld(&s->mb[addr - CAN_MB_BASE], size);
    } else if (addr >= CAN_MKR_BASE && addr < CAN_MKR_BASE + CAN_MKR_SIZE) {
        val = can_ld(&s->mkr[addr - CAN_MKR_BASE], size);
    } else if (addr == CAN_STR && size == 2) {
        val = can_str(s);
    } else if (addr >= CAN_CTL_BASE && addr < CAN_CTL_BASE + CAN_CTL_SIZE) {
        val = can_ld(&s->ctl[addr - CAN_CTL_BASE], size);
    } else {
        qemu_log_mask(LOG_UNIMP, "renesas-can: читання поза мапою +%#"
                      HWADDR_PRIx " (%u)\n", addr, size);
        val = 0;
    }

    can_ctx(s, ctx, sizeof(ctx));
    trace_renesas_can_read(addr, size, val, ctx);
    return val;
}

static void can_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    RenesasCANState *s = opaque;
    char ctx[80];

    can_ctx(s, ctx, sizeof(ctx));
    trace_renesas_can_write(addr, size, val, ctx);

    if (addr < CAN_MB_BASE + CAN_MB_SIZE) {
        unsigned i;

        for (i = 0; i < size; i++) {
            hwaddr a = addr - CAN_MB_BASE + i;
            uint8_t b = val >> (8 * i);

            /*
             * Другий байт скриньки — старша половина 16-бітного слова за
             * +0x00, і його біт 5 (тобто біт 13 слова) нікуди не пишеться:
             * саме цей біт драйвер виключає зі своєї самоперевірки.
             */
            if ((a & 0xf) == 1) {
                b &= 0xdf;
            }
            s->mb[a] = b;
        }
        return;
    }
    if (addr >= CAN_MKR_BASE && addr < CAN_MKR_BASE + CAN_MKR_SIZE) {
        can_st(&s->mkr[addr - CAN_MKR_BASE], size, val);
        return;
    }
    if (addr == CAN_STR && size == 2) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "renesas-can: запис у STR (лише читання) %#" PRIx64 "\n",
                      val);
        return;
    }
    if (addr >= CAN_CTL_BASE && addr < CAN_CTL_BASE + CAN_CTL_SIZE) {
        can_st(&s->ctl[addr - CAN_CTL_BASE], size, val);
        if (addr <= CAN_CTLR && addr + size > CAN_CTLR) {
            can_ctlr_write(s, can_ld(&s->ctl[CTL_OFF(CAN_CTLR)], 2));
        }
        return;
    }

    qemu_log_mask(LOG_UNIMP, "renesas-can: запис поза мапою +%#" HWADDR_PRIx
                  " (%u) = %#" PRIx64 "\n", addr, size, val);
}

static const MemoryRegionOps can_ops = {
    .read = can_read,
    .write = can_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void can_reset_hold(Object *obj, ResetType type)
{
    RenesasCANState *s = RENESAS_CAN(obj);

    memset(s->mb, 0, sizeof(s->mb));
    memset(s->mkr, 0, sizeof(s->mkr));
    memset(s->ctl, 0, sizeof(s->ctl));

    /* Після ввімкнення живлення контролер стоїть у режимі скидання. */
    s->mode = CAN_MODE_RESET;
    s->sleep = false;
    can_st(&s->ctl[CTL_OFF(CAN_CTLR)], 2, CTLR_CANM_RESET);
    /* Пошуку скриньок не було — результату немає. */
    s->ctl[CTL_MSSR] = MSSR_SEST;
}

static void can_init(Object *obj)
{
    RenesasCANState *s = RENESAS_CAN(obj);

    s->trace_pc = getenv("QY8_CAN_PC") != NULL;
    memory_region_init_io(&s->iomem, obj, &can_ops, s, "renesas-can",
                          RENESAS_CAN_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static const VMStateDescription vmstate_renesas_can = {
    .name = "renesas-can",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(mb, RenesasCANState, CAN_MB_SIZE),
        VMSTATE_UINT8_ARRAY(mkr, RenesasCANState, CAN_MKR_SIZE),
        VMSTATE_UINT8_ARRAY(ctl, RenesasCANState, CAN_CTL_SIZE),
        VMSTATE_UINT8(mode, RenesasCANState),
        VMSTATE_BOOL(sleep, RenesasCANState),
        VMSTATE_END_OF_LIST()
    },
};

static void can_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "Renesas CAN controller (R-Car Gen1)";
    dc->vmsd = &vmstate_renesas_can;
    rc->phases.hold = can_reset_hold;
}

static const TypeInfo renesas_can_types[] = {
    {
        .name          = TYPE_RENESAS_CAN,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(RenesasCANState),
        .instance_init = can_init,
        .class_init    = can_class_init,
    },
};

DEFINE_TYPES(renesas_can_types)
