/*
 * Renesas HPB-DMAC (High Performance Bus DMA controller, R8A7778) —
 * модель для плати Clarion QY8XXX (Nissan Leaf ZE1).
 *
 * Блок ототожнено не за схожістю адрес, а за тим, що збіглася ВСЯ
 * регістрова мапа: гість пише сімку регістрів із кроком 0x40, і кожен
 * зсув лягає на drivers/dma/sh/rcar-hpbdma.c один-в-один. Бази й
 * переривання — з arch/arm/mach-shmobile/setup-r8a7778.c
 * (hpb_dmae_resources[]), тобто з платформенного файлу САМЕ нашого SoC:
 *
 *   канальні регістри 0xffc08000/0x1000, спільні 0xffc09000/0x170,
 *   async reset 0xffc00300, async mode 0xffc00400,
 *   переривання gic_iid(0x7b) — 5 ліній, IRQ 123..127.
 *
 * Навіщо це ядру WinCE. Прошивка піднімає два канали на SCIF4:
 *
 *   канал 4: DSAR0/DSAR1 = 0xffe44014 (SCIF4 SCFRDR), DDAR0/DDAR1 —
 *            два буфери в ОЗП по 0x80 Б, DCR = CT|DIP|SMDL
 *            (безперервний прийом у подвійний буфер);
 *   канал 6: DSAR0 = буфер в ОЗП, DDAR0 = 0xffe4400c (SCIF4 SCFTDR),
 *            DTCR0 = 8, DCR = DMDL (передача).
 *
 * У буфері передачі лежить 10 02 | 02 08 00 | 10 03 | 19 — кадр DLE STX
 * / дані / DLE ETX / сума. Тобто SCIF4 — це лінк до підпорядкованого
 * мікроконтролера плати (у прошивці є cpucom.dll і ціла родина
 * *MicomUpdate.exe), а не налагоджувальна консоль.
 *
 * ⚠ Межа чесності. Напрямок «пам'ять -> модуль» виконується одразу:
 * наш SCIF завжди готовий приймати, тож миттєва передача — це не
 * спрощення, а точний опис цієї моделі. Напрямок «модуль -> пам'ять»
 * НЕ виконується наосліп: канал лише озброюється, і байти в нього
 * подає сам SCIF через clarion_hpbdma_feed() — інакше модель вигадала
 * б дані, яких на шині немає. Поки на SCIF4 ніхто не відповідає,
 * канал прийому чесно стоїть активним і нічого не переносить.
 *
 * Чого свідомо немає (джерела не кажуть — не вигадуємо):
 *   DPTR: прошивка пише туди 4 і 0x400, драйвер Linux його не чіпає —
 *         зберігаємо, не тлумачимо;
 *   DTIMR, DMASPR, DMLVLR, DMSHPT: у драйвері лише названі;
 *   апаратна швидкість передачі — переносимо все за один такт.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/dma/clarion_hpbdma.h"
#include "system/address-spaces.h"
#include "qom/object.h"

/* --- канальні регістри (зсуви з rcar-hpbdma.c) ------------------------ */

#define HPB_DSAR0       0x00
#define HPB_DDAR0       0x04
#define HPB_DTCR0       0x08
#define HPB_DSAR1       0x0C
#define HPB_DDAR1       0x10
#define HPB_DTCR1       0x14
#define HPB_DSASR       0x18
#define HPB_DDASR       0x1C
#define HPB_DTCSR       0x20
#define HPB_DPTR        0x24
#define HPB_DCR         0x28
#define HPB_DCMDR       0x2C
#define HPB_DSTPR       0x30
#define HPB_DSTSR       0x34
#define HPB_DDBGR       0x38
#define HPB_DDBGR2      0x3C
#define HPB_CHAN_STRIDE 0x40

/* DCMDR */
#define DCMDR_BDOUT     (1u << 7)
#define DCMDR_DQSPD     (1u << 6)
#define DCMDR_DQSPC     (1u << 5)
#define DCMDR_DMSPD     (1u << 4)
#define DCMDR_DMSPC     (1u << 3)
#define DCMDR_DQEND     (1u << 2)
#define DCMDR_DNXT      (1u << 1)
#define DCMDR_DMEN      (1u << 0)

/* DSTPR / DSTSR */
#define DSTPR_DMSTP     (1u << 0)
#define DSTSR_DQSTS     (1u << 2)
#define DSTSR_DMSTS     (1u << 0)

/* DCR (include/linux/platform_data/dma-rcar-hpbdma.h) */
#define DCR_CT          (1u << 18)
#define DCR_DIP         (1u << 16)
#define DCR_SMDL        (1u << 13)
#define DCR_SPDS_MASK   (3u << 8)
#define DCR_SPDS_SHIFT  8
#define DCR_DMDL        (1u << 5)
#define DCR_DPDS_MASK   (3u << 0)
#define DCR_DPDS_SHIFT  0

/* --- спільні регістри ------------------------------------------------- */

#define HPB_DTIMR       0x00
#define HPB_DINTSR0     0x0C
#define HPB_DINTSR1     0x10
#define HPB_DINTCR0     0x14
#define HPB_DINTCR1     0x18
#define HPB_DINTMR0     0x1C
#define HPB_DINTMR1     0x20
#define HPB_DACTSR0     0x24
#define HPB_DACTSR1     0x28
#define HPB_HSRSTR(n)   (0x40 + (n) * 4)

typedef struct ClarionHpbChan {
    uint32_t sar[2], dar[2], tcr[2];    /* дві «площини» (DIP) */
    uint32_t dsasr, ddasr, dtcsr;
    uint32_t dptr, dcr, dstsr;
    uint32_t left;                      /* скільки одиниць лишилося */
    unsigned plane;                     /* яка площина зараз */
    bool active;
} ClarionHpbChan;

struct ClarionHpbDmaState {
    SysBusDevice parent_obj;

    MemoryRegion chan_mr;
    MemoryRegion comm_mr;
    qemu_irq irq[CLARION_HPBDMA_NUM_IRQ];

    ClarionHpbChan ch[CLARION_HPBDMA_NUM_CHAN];
    uint32_t dintsr[2];                 /* стан переривань */
    uint32_t dintmr[2];                 /* маски */
    uint32_t dtimr;
};

OBJECT_DECLARE_SIMPLE_TYPE(ClarionHpbDmaState, CLARION_HPBDMA)

/*
 * Канал -> лінія переривання.
 *
 * Таблиця hpb_dmae_channels[] у setup-r8a7778.c дає ch_irq для тих
 * каналів, які використовує Linux: 0x7c (IRQ 124) для 14/15 (USB-функція),
 * 0x7e (IRQ 126) для 21/22 (SDHI0), 0x7f (IRQ 127) для 28..36 (SSI/HPBIF).
 * Каналів 4 і 6 у Linux немає — але їх прив'язує сам гість: він
 * користується каналами 4, 6 (micom) і 21, 22 (SDHI) і вмикає в GIC
 * рівно IRQ 123 та 126. 126 належить SDHI за таблицею вище, отже 123 —
 * це канали 4/6, тобто найнижча з п'яти ліній.
 *
 * Для решти каналів джерела немає; віддаємо лінію 0 і кажемо про це в лог.
 */
static int hpb_chan_irq_line(ClarionHpbDmaState *s, int ch)
{
    if (ch == 14 || ch == 15) {
        return 1;                       /* 0x7c = IRQ 124 */
    }
    if (ch == 21 || ch == 22) {
        return 3;                       /* 0x7e = IRQ 126 */
    }
    if (ch >= 28 && ch <= 36) {
        return 4;                       /* 0x7f = IRQ 127 */
    }
    if (ch != 4 && ch != 6) {
        qemu_log_mask(LOG_UNIMP,
                      "clarion-hpbdma: канал %d — лінії переривання для "
                      "нього немає в жодному джерелі, беремо нижню\n", ch);
    }
    return 0;                           /* gic_iid(0x7b) = IRQ 123 */
}

static void hpb_update_irq(ClarionHpbDmaState *s)
{
    bool raised[CLARION_HPBDMA_NUM_IRQ] = { false };
    int ch, i;

    for (ch = 0; ch < CLARION_HPBDMA_NUM_CHAN; ch++) {
        int w = ch / 32, b = ch % 32;

        if ((s->dintsr[w] & s->dintmr[w]) & (1u << b)) {
            raised[hpb_chan_irq_line(s, ch)] = true;
        }
    }
    for (i = 0; i < CLARION_HPBDMA_NUM_IRQ; i++) {
        qemu_set_irq(s->irq[i], raised[i]);
    }
}

static void hpb_chan_complete(ClarionHpbDmaState *s, int ch)
{
    int w = ch / 32, b = ch % 32;

    s->dintsr[w] |= 1u << b;
    hpb_update_irq(s);
}

/* Розмір одиниці передачі: SPDS/DPDS, 0=8 біт, 1=16, 2=32. */
static unsigned hpb_unit(ClarionHpbDmaState *s, int ch)
{
    uint32_t dcr = s->ch[ch].dcr;
    unsigned spds = (dcr & DCR_SPDS_MASK) >> DCR_SPDS_SHIFT;
    unsigned dpds = (dcr & DCR_DPDS_MASK) >> DCR_DPDS_SHIFT;

    if (spds != dpds) {
        qemu_log_mask(LOG_UNIMP,
                      "clarion-hpbdma: канал %d має різну ширину портів "
                      "(SPDS=%u, DPDS=%u) — беремо ширшу\n", ch, spds, dpds);
    }
    return 1u << MAX(spds > 2 ? 2 : spds, dpds > 2 ? 2 : dpds);
}

/*
 * Перенести одну одиницю. Бік, позначений «MDL», — це модуль (периферія),
 * його адреса не рухається; інший бік інкрементується.
 */
static void hpb_move_one(ClarionHpbDmaState *s, int ch)
{
    ClarionHpbChan *c = &s->ch[ch];
    unsigned unit = hpb_unit(s, ch);
    uint8_t buf[4];

    address_space_read(&address_space_memory, c->dsasr,
                       MEMTXATTRS_UNSPECIFIED, buf, unit);
    address_space_write(&address_space_memory, c->ddasr,
                        MEMTXATTRS_UNSPECIFIED, buf, unit);

    if (!(c->dcr & DCR_SMDL)) {
        c->dsasr += unit;
    }
    if (!(c->dcr & DCR_DMDL)) {
        c->ddasr += unit;
    }
    c->left--;
    c->dtcsr = c->left;
}

static void hpb_chan_load_plane(ClarionHpbDmaState *s, int ch, unsigned plane)
{
    ClarionHpbChan *c = &s->ch[ch];

    c->plane = plane;
    c->dsasr = c->sar[plane];
    c->ddasr = c->dar[plane];
    c->left = c->tcr[plane];
    c->dtcsr = c->left;
}

/* ТИМЧАСОВО: журнал роботи каналів під QY8_DMA_LOG (знести після досліду) */
static bool hpb_dbg(void)
{
    static int on = -1;
    if (on < 0) {
        on = getenv("QY8_DMA_LOG") != NULL;
    }
    return on;
}

static void hpb_chan_start(ClarionHpbDmaState *s, int ch, bool next)
{
    ClarionHpbChan *c = &s->ch[ch];
    unsigned plane = 0;

    if ((c->dcr & DCR_DIP) && next) {
        plane = c->plane ^ 1;
    }
    hpb_chan_load_plane(s, ch, plane);
    c->active = true;
    if (hpb_dbg() && (ch == 4 || ch == 6)) {
        fprintf(stderr, "[dma] start ch%d plane%u dcr=%08x dar=%08x tcr=%u\n",
                ch, plane, c->dcr, c->ddasr, c->left);
    }
    c->dstsr |= DSTSR_DMSTS;

    if ((c->dcr & DCR_SMDL) && !(c->dcr & DCR_DMDL)) {
        /*
         * Модуль -> пам'ять. Даних у нас немає — канал просто озброєно,
         * байти в нього подасть SCIF через clarion_hpbdma_feed().
         */
        return;
    }
    if ((c->dcr & DCR_SMDL) && (c->dcr & DCR_DMDL)) {
        qemu_log_mask(LOG_UNIMP, "clarion-hpbdma: канал %d — модуль у модуль, "
                      "джерела на таку передачу немає\n", ch);
        return;
    }

    /* Пам'ять -> модуль (або пам'ять -> пам'ять): наш приймач завжди готовий. */
    while (c->left) {
        hpb_move_one(s, ch);
    }
    c->active = false;
    c->dstsr &= ~DSTSR_DMSTS;
    hpb_chan_complete(s, ch);
}

/*
 * Байт прийшов на периферійний регістр periph_addr. Якщо його чекає
 * озброєний канал — покласти в пам'ять і, коли лічильник добіг,
 * підняти переривання. Повертає true, якщо байт забрав DMA.
 */
bool clarion_hpbdma_feed(DeviceState *dev, hwaddr periph_addr, uint8_t val)
{
    ClarionHpbDmaState *s = CLARION_HPBDMA(dev);
    int ch;

    for (ch = 0; ch < CLARION_HPBDMA_NUM_CHAN; ch++) {
        ClarionHpbChan *c = &s->ch[ch];

        if (!c->active || !(c->dcr & DCR_SMDL) || c->dsasr != periph_addr) {
            continue;
        }
        address_space_write(&address_space_memory, c->ddasr,
                            MEMTXATTRS_UNSPECIFIED, &val, 1);
        c->ddasr++;
        c->left--;
        c->dtcsr = c->left;

        if (!c->left) {
            if (hpb_dbg()) {
                fprintf(stderr, "[dma] ch%d буфер добіг (plane%u)\n",
                        ch, c->plane);
            }
            hpb_chan_complete(s, ch);
            if (c->dcr & DCR_CT) {
                /* Безперервний режим: одразу наступна площина. */
                hpb_chan_load_plane(s, ch,
                                    (c->dcr & DCR_DIP) ? c->plane ^ 1 : 0);
            } else {
                c->active = false;
                c->dstsr &= ~DSTSR_DMSTS;
            }
        }
        return true;
    }
    return false;
}

/* --- канальні регістри ------------------------------------------------ */

static uint64_t hpb_chan_read(void *opaque, hwaddr addr, unsigned size)
{
    ClarionHpbDmaState *s = opaque;
    int ch = addr / HPB_CHAN_STRIDE;
    hwaddr off = addr % HPB_CHAN_STRIDE;

    if (ch >= CLARION_HPBDMA_NUM_CHAN) {
        return 0;
    }

    if (hpb_dbg() && ch == 4 &&
        (off == HPB_DDASR || off == HPB_DTCSR || off == HPB_DSTSR)) {
        fprintf(stderr, "[dma] ЧИТАННЯ ch4 +%02x (ddasr=%08x left=%u)\n",
                (unsigned)off, s->ch[ch].ddasr, s->ch[ch].left);
    }
    switch (off) {
    case HPB_DSAR0: return s->ch[ch].sar[0];
    case HPB_DDAR0: return s->ch[ch].dar[0];
    case HPB_DTCR0: return s->ch[ch].tcr[0];
    case HPB_DSAR1: return s->ch[ch].sar[1];
    case HPB_DDAR1: return s->ch[ch].dar[1];
    case HPB_DTCR1: return s->ch[ch].tcr[1];
    case HPB_DSASR: return s->ch[ch].dsasr;
    case HPB_DDASR: return s->ch[ch].ddasr;
    case HPB_DTCSR: return s->ch[ch].dtcsr;
    case HPB_DPTR:  return s->ch[ch].dptr;
    case HPB_DCR:   return s->ch[ch].dcr;
    case HPB_DCMDR: return 0;           /* команда, читається нулем */
    case HPB_DSTPR: return 0;
    case HPB_DSTSR: return s->ch[ch].dstsr;
    default:        return 0;
    }
}

static void hpb_chan_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned size)
{
    ClarionHpbDmaState *s = opaque;
    int ch = addr / HPB_CHAN_STRIDE;
    hwaddr off = addr % HPB_CHAN_STRIDE;

    if (ch >= CLARION_HPBDMA_NUM_CHAN) {
        return;
    }

    switch (off) {
    case HPB_DSAR0: s->ch[ch].sar[0] = val; break;
    case HPB_DDAR0: s->ch[ch].dar[0] = val; break;
    case HPB_DTCR0: s->ch[ch].tcr[0] = val; break;
    case HPB_DSAR1: s->ch[ch].sar[1] = val; break;
    case HPB_DDAR1: s->ch[ch].dar[1] = val; break;
    case HPB_DTCR1: s->ch[ch].tcr[1] = val; break;
    case HPB_DPTR:  s->ch[ch].dptr = val; break;   /* зберігаємо, не тлумачимо */
    case HPB_DCR:   s->ch[ch].dcr = val; break;

    case HPB_DCMDR:
        if (val & DCMDR_DMEN) {
            hpb_chan_start(s, ch, !!(val & DCMDR_DNXT));
        }
        if (val & (DCMDR_DQEND | DCMDR_DQSPD | DCMDR_DMSPD)) {
            s->ch[ch].active = false;
            s->ch[ch].dstsr &= ~DSTSR_DMSTS;
        }
        break;

    case HPB_DSTPR:
        if (val & DSTPR_DMSTP) {
            s->ch[ch].active = false;
            s->ch[ch].dstsr &= ~DSTSR_DMSTS;
        }
        break;

    default:
        break;
    }
}

static const MemoryRegionOps hpb_chan_ops = {
    .read = hpb_chan_read,
    .write = hpb_chan_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

/* --- спільні регістри ------------------------------------------------- */

static uint64_t hpb_comm_read(void *opaque, hwaddr addr, unsigned size)
{
    ClarionHpbDmaState *s = opaque;
    uint32_t act[2] = { 0, 0 };
    int ch;

    switch (addr) {
    case HPB_DTIMR:   return s->dtimr;
    case HPB_DINTSR0: return s->dintsr[0];
    case HPB_DINTSR1: return s->dintsr[1];
    case HPB_DINTMR0: return s->dintmr[0];
    case HPB_DINTMR1: return s->dintmr[1];
    case HPB_DACTSR0:
    case HPB_DACTSR1:
        for (ch = 0; ch < CLARION_HPBDMA_NUM_CHAN; ch++) {
            if (s->ch[ch].active) {
                act[ch / 32] |= 1u << (ch % 32);
            }
        }
        return addr == HPB_DACTSR0 ? act[0] : act[1];
    default:
        return 0;
    }
}

static void hpb_comm_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned size)
{
    ClarionHpbDmaState *s = opaque;
    int ch;

    switch (addr) {
    case HPB_DTIMR:
        s->dtimr = val;
        break;
    case HPB_DINTCR0:                   /* запис одиниці скидає стан */
        s->dintsr[0] &= ~(uint32_t)val;
        hpb_update_irq(s);
        break;
    case HPB_DINTCR1:
        s->dintsr[1] &= ~(uint32_t)val;
        hpb_update_irq(s);
        break;
    case HPB_DINTMR0:
        s->dintmr[0] = val;
        hpb_update_irq(s);
        break;
    case HPB_DINTMR1:
        s->dintmr[1] = val;
        hpb_update_irq(s);
        break;
    default:
        /* HSRSTR(n): скидання каналу n (hpb_dmae_reset) */
        if (addr >= HPB_HSRSTR(0) &&
            addr < HPB_HSRSTR(CLARION_HPBDMA_NUM_CHAN) && (val & 1)) {
            ch = (addr - HPB_HSRSTR(0)) / 4;
            memset(&s->ch[ch], 0, sizeof(s->ch[ch]));
            s->dintsr[ch / 32] &= ~(1u << (ch % 32));
            hpb_update_irq(s);
        }
        break;
    }
}

static const MemoryRegionOps hpb_comm_ops = {
    .read = hpb_comm_read,
    .write = hpb_comm_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

/* --- пристрій --------------------------------------------------------- */

static void clarion_hpbdma_reset_hold(Object *obj, ResetType type)
{
    ClarionHpbDmaState *s = CLARION_HPBDMA(obj);

    memset(s->ch, 0, sizeof(s->ch));
    s->dintsr[0] = s->dintsr[1] = 0;
    s->dintmr[0] = s->dintmr[1] = 0;
    s->dtimr = 0;
    hpb_update_irq(s);
}

static void clarion_hpbdma_realize(DeviceState *dev, Error **errp)
{
    ClarionHpbDmaState *s = CLARION_HPBDMA(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    int i;

    memory_region_init_io(&s->chan_mr, OBJECT(dev), &hpb_chan_ops, s,
                          "clarion-hpbdma.chan", CLARION_HPBDMA_CHAN_SIZE);
    memory_region_init_io(&s->comm_mr, OBJECT(dev), &hpb_comm_ops, s,
                          "clarion-hpbdma.comm", CLARION_HPBDMA_COMM_SIZE);
    sysbus_init_mmio(sbd, &s->chan_mr);
    sysbus_init_mmio(sbd, &s->comm_mr);

    for (i = 0; i < CLARION_HPBDMA_NUM_IRQ; i++) {
        sysbus_init_irq(sbd, &s->irq[i]);
    }
}

static void clarion_hpbdma_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = clarion_hpbdma_realize;
    dc->desc = "Renesas HPB-DMAC (R-Car Gen1)";
    rc->phases.hold = clarion_hpbdma_reset_hold;
}

static const TypeInfo clarion_hpbdma_types[] = {
    {
        .name          = TYPE_CLARION_HPBDMA,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(ClarionHpbDmaState),
        .class_init    = clarion_hpbdma_class_init,
    },
};

DEFINE_TYPES(clarion_hpbdma_types)
