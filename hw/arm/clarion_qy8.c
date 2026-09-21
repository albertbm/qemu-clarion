/*
 * Clarion QY8XXX head unit (Nissan Leaf ZE1) — емуляція плати.
 *
 * SoC: Renesas R8A7778 (R-Car M1A), одноядерний Cortex-A9.
 * ОС:  Windows CE, XIP просто з флеш на CS0 (reset-вектор @PA 0).
 *
 * Карта пам'яті знята з OEMAddressTable самого завантажувача
 * (nand_20260827.bin @0x11c2c), а не вгадана:
 *
 *   VA 80000000 -> PA 00000000  64 MB   флеш CS0 (той самий дамп)
 *   VA 84000000 -> PA 04000000   8 MB   CS1
 *   VA 84800000 -> PA 18000000   8 MB
 *   VA 85000000 -> PA 18800000   8 MB
 *   VA 85800000 -> PA f0000000   8 MB   L2C (PL310 @f0100000)
 *   VA 88000000 -> PA 08000000 128 MB   DDR (сюди вантажиться ядро)
 *   VA 90000000 -> PA 10000000 128 MB   DDR (тут виконується eboot)
 *   VA 9c000000 -> PA fc000000  64 MB   периферія
 *
 * Периферія — стандартні блоки Renesas; адреси збігаються з r8a7778.dtsi
 * і з константами, що лежать у коді eboot.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/core/boards.h"
#include "hw/core/sysbus.h"
#include "hw/core/loader.h"
#include "hw/core/qdev-properties.h"
#include "hw/arm/boot.h"
#include "hw/intc/arm_gic.h"
#include "hw/display/clarion_du.h"
#include "hw/dma/clarion_hpbdma.h"
#include "hw/misc/unimp.h"
#include "system/system.h"
#include "system/address-spaces.h"
#include "chardev/char-fe.h"
#include "hw/core/ptimer.h"
#include "hw/core/irq.h"
#include "target/arm/cpu-qom.h"
#include "qom/object.h"

/* --- фізична карта плати --------------------------------------------- */

#define QY8_FLASH_BASE      0x00000000
#define QY8_FLASH_SIZE      (64 * MiB)

#define QY8_CS1_BASE        0x04000000
#define QY8_CS1_SIZE        (8 * MiB)

#define QY8_DDR0_BASE       0x08000000      /* образ ядра, VA 0x88000000 */
#define QY8_DDR0_SIZE       (128 * MiB)
#define QY8_DDR1_BASE       0x10000000      /* eboot, VA 0x90000000      */
#define QY8_DDR1_SIZE       (128 * MiB)

#define QY8_SRAM0_BASE      0x18000000
#define QY8_SRAM0_SIZE      (8 * MiB)
#define QY8_SRAM1_BASE      0x18800000
#define QY8_SRAM1_SIZE      (8 * MiB)

/* eboot виконується звідси (VA 0x97C00000 -> PA 0x17C00000) */
#define QY8_EBOOT_PA        0x17C00000

/* GIC — як у r8a7778.dtsi; обидві бази присутні в коді eboot */
#define QY8_GIC_CPU_BASE    0xFE430000
#define QY8_GIC_DIST_BASE   0xFE438000
#define QY8_NUM_IRQ         256

#define QY8_L2C_BASE        0xF0100000

/*
 * Display Unit — display@fff80000 reg = <0xfff80000 0x40000> (r8a7779.dtsi).
 * Модель — hw/display/clarion_du.c. Дисплей програмує ЗАВАНТАЖУВАЧ: він
 * вмикає 800x480 на каналі 0 і лишає DOOR = 0. Регістрів площини він не
 * пише взагалі, тож вікно до M3b лишається чорним — це стан системи, а не
 * вада моделі (docs/20, розділ «M4»).
 */
#define QY8_DU_BASE         0xFFF80000
#define QY8_DU_SPI          31          /* interrupts = <GIC_SPI 31> */

#define QY8_SCIF_BASE       0xFFE40000      /* scif0..scif5, крок 0x1000 */
#define QY8_SCIF_STRIDE     0x1000
#define QY8_NUM_SCIF        6

/*
 * Консоль завантажувача — SCIF3 (0xFFE43000), а не SCIF0. Це не здогад:
 * OEMInitDebugSerial @VA 0x97c14758 будує базу двома інструкціями
 * (mov r3,#0x3000; sub r3,r3,#0x1c0000 -> 0xFFE43000) і кладе її в глобал
 * 0x97cc249c, з якого putchar @0x97c14784 бере base+0x10 (SCFSR.TDFE) і
 * base+0x0c (SCFTDR). Тому перший -serial (при -nographic це stdio)
 * чіпляємо саме до SCIF3, решту — до наступних за порядком.
 */
#define QY8_SCIF_DEBUG      3

/* serial@ffe4n000 interrupts = <GIC_SPI 70+n> (r8a7778.dtsi) */
#define QY8_SCIF_SPI0       70

static int qy8_scif_chr_index(int scif)
{
    return scif == QY8_SCIF_DEBUG ? 0 :
           scif < QY8_SCIF_DEBUG ? scif + 1 : scif;
}

/* --- контролер плати на CS-шині @0x18800000 --------------------------- */

#define QY8_BCTL_BASE       0x18800000
#define QY8_BCTL_SIZE       0x40

#define BCTL_STATUS         0x00
#define BCTL_DIPSW          0x02

/* Біти статусу, які опитує завантажувач у циклі @0x1148 (див. нижче). */
#define BCTL_ST_PWR         0x0080
#define BCTL_ST_BOOT        0x0400

/* Режими DIPSW — таблиця переходів eboot @VA 0x97c07e28 */
#define QY8_DIPSW_NORM_RES  5           /* "NORM(RES)>>" — як із TEST_B1 */

/* --- SCIF (Renesas serial, 16-бітні регістри) ------------------------- */

#define SCIF_SCSMR      0x00
#define SCIF_SCBRR      0x04
#define SCIF_SCSCR      0x08
#define SCIF_SCFTDR     0x0C
#define SCIF_SCFSR      0x10
#define SCIF_SCFRDR     0x14
#define SCIF_SCFCR      0x18
#define SCIF_SCFDR      0x1C
#define SCIF_SCSPTR     0x20
#define SCIF_SCLSR      0x24

/* SCFSR (мапа sh-sci.h: DR, RDF, PER, FER, BRK, TDFE, TEND, ER) */
#define SCFSR_DR        0x0001
#define SCFSR_RDF       0x0002
#define SCFSR_TDFE      0x0020
#define SCFSR_TEND      0x0040

/* SCFCR: RTRG[7:6] — рівень запуску приймача, RFRST — скидання FIFO прийому */
#define SCFCR_RFRST     0x0002
#define QY8_SCIF_FIFO   16

/*
 * DR («receive data ready») SCIF зводить, коли у FIFO є байти, їх менше за
 * рівень запуску, і лінія мовчить ~15 бітових інтервалів. Швидкість гість
 * задає не через SCBRR (пише туди 0), тож беремо сталу паузу — важлива не
 * її точність, а сам факт «посилка скінчилася».
 */
#define QY8_SCIF_IDLE_NS    200000

typedef struct Qy8Scif {
    MemoryRegion mr;
    CharFrontend chr;
    qemu_irq irq;
    DeviceState *dmac;          /* кому віддавати прийняті байти */
    hwaddr base;                /* фізична база — щоб назвати SCFRDR для DMA */
    uint16_t scsmr, scscr, scfcr;
    uint8_t fifo[QY8_SCIF_FIFO];
    unsigned fifo_len;
    bool dr;                    /* прийом завершився паузою */
    QEMUTimer *idle;
    int index;
} Qy8Scif;

/* SCSCR — дозволи переривань (мапа sh-sci.h) */
#define SCSCR_RIE       0x0040
#define SCSCR_TIE       0x0080

/* RTRG[7:6] -> скільки байтів у FIFO запускають запит DMA (мапа SCIF) */
static unsigned qy8_scif_rtrg(Qy8Scif *s)
{
    static const unsigned lvl[4] = { 1, 4, 8, 14 };
    return lvl[(s->scfcr >> 6) & 3];
}

static bool qy8_scif_rdf(Qy8Scif *s)
{
    return s->fifo_len >= qy8_scif_rtrg(s);
}

static void qy8_scif_update_irq(Qy8Scif *s)
{
    /*
     * Переривання приймача просять RDF і DR, і обидва — лише при RIE.
     * ⚠ Перевірено дослідом: якщо підняти лінію по DR без RIE (є спокуса
     * тлумачити біт 2 SCSCR як TOIE з sh-sci.h), драйвер SCIF4 входить в
     * обробник, читає SCFSR/SCSCR/SCLSR, нічого не бере з SCFRDR і не гасить
     * причину — виходить нескінченний шторм (219 тис. входів за 16 с). Тобто
     * такого джерела на цьому SCIF драйвер не знає. Передавач у нас завжди
     * порожній, тож TIE не зводимо — інакше лінія висіла б вічно.
     */
    qemu_set_irq(s->irq, (qy8_scif_rdf(s) || s->dr) && (s->scscr & SCSCR_RIE));
}

/*
 * Віддати байти DMA. Запит DMA приймача — це той самий RXI, і SCIF зводить
 * його або по RDF (набрався рівень запуску RTRG), або по DR (посилка
 * скінчилася, у FIFO лишилося менше за рівень). Що RDF і DR — одне й те саме
 * джерело RXI, видно з `sh-sci.c`: в `sci_rx_interrupt()` драйвер гасить
 * причину як `ssr & ~(SCIF_DR | SCxSR_RDxF(port))`, тобто обидва біти разом.
 * Що цей самий RXI йде в DMAC, у джерелах Linux не записано (каналів SCIF у
 * `hpb_dmae_slaves[]` немає) — це рішення моделі; без нього коротка
 * відповідь micom назавжди лишалася б у FIFO, чого на живій платі не буває.
 */
static void qy8_scif_rx_pump(Qy8Scif *s)
{
    unsigned taken = 0;

    if (s->dmac && (s->fifo_len >= qy8_scif_rtrg(s) || s->dr)) {
        while (taken < s->fifo_len &&
               clarion_hpbdma_feed(s->dmac, s->base + SCIF_SCFRDR,
                                   s->fifo[taken])) {
            taken++;
        }
    }
    if (taken) {
        memmove(s->fifo, s->fifo + taken, s->fifo_len - taken);
        s->fifo_len -= taken;
        if (!s->fifo_len) {
            s->dr = false;
        }
    }
    qy8_scif_update_irq(s);
}

static void qy8_scif_idle_expire(void *opaque)
{
    Qy8Scif *s = opaque;

    if (s->fifo_len) {
        s->dr = true;
        qy8_scif_rx_pump(s);        /* DR теж просить DMA — див. rx_pump */
    }
    if (s->fifo_len) {
        /*
         * Канал DMA зараз не озброєний — запит лишається висіти, як у
         * залізі, і байти чекають у FIFO. Перевіряємо ще раз згодом:
         * інакше про них ніхто б не згадав до наступного прийнятого байта.
         */
        timer_mod(s->idle, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                           QY8_SCIF_IDLE_NS);
    }
}

static uint64_t qy8_scif_read(void *opaque, hwaddr addr, unsigned size)
{
    Qy8Scif *s = opaque;

    switch (addr) {
    case SCIF_SCSMR:
        return s->scsmr;
    case SCIF_SCSCR:
        return s->scscr;
    case SCIF_SCFSR:
        /* передавач завжди готовий; приймач — за станом FIFO */
        return SCFSR_TDFE | SCFSR_TEND |
               (qy8_scif_rdf(s) ? SCFSR_RDF : 0) | (s->dr ? SCFSR_DR : 0);
    case SCIF_SCFRDR: {
        uint8_t v = s->fifo_len ? s->fifo[0] : 0;

        if (s->fifo_len) {
            memmove(s->fifo, s->fifo + 1, --s->fifo_len);
        }
        if (!s->fifo_len) {
            s->dr = false;
        }
        qy8_scif_update_irq(s);
        return v;
    }
    case SCIF_SCFCR:
        return s->scfcr;
    case SCIF_SCFDR:
        /* старший байт — заповненість TX FIFO (0), молодший — RX */
        return s->fifo_len;
    case SCIF_SCSPTR:
        return 0;
    case SCIF_SCLSR:
        return 0;
    default:
        return 0;
    }
}

static void qy8_scif_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned size)
{
    Qy8Scif *s = opaque;
    uint8_t ch;

    switch (addr) {
    case SCIF_SCSMR:
        s->scsmr = val;
        break;
    case SCIF_SCSCR:
        s->scscr = val;
        qy8_scif_update_irq(s);
        break;
    case SCIF_SCFTDR:
        ch = val & 0xff;
        /* синхронний вивід: без нього ранні рядки буту губляться */
        qemu_chr_fe_write_all(&s->chr, &ch, 1);
        break;
    case SCIF_SCFCR:
        s->scfcr = val;
        if (val & SCFCR_RFRST) {            /* скидання FIFO прийому */
            s->fifo_len = 0;
            s->dr = false;
        }
        qy8_scif_update_irq(s);
        break;
    case SCIF_SCFSR:
        /* прапорці скидаються записом нуля у відповідний біт */
        if (!(val & SCFSR_DR)) {
            s->dr = false;
        }
        qy8_scif_update_irq(s);
        break;
    default:
        break;
    }
}

static const MemoryRegionOps qy8_scif_ops = {
    .read = qy8_scif_read,
    .write = qy8_scif_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static int qy8_scif_can_receive(void *opaque)
{
    Qy8Scif *s = opaque;
    return QY8_SCIF_FIFO - s->fifo_len;
}

static void qy8_scif_receive(void *opaque, const uint8_t *buf, int size)
{
    Qy8Scif *s = opaque;

    /*
     * Байти лягають у FIFO приймача, як у залізі. DMA забирає їх лише коли
     * назбирався рівень запуску RTRG (гість ставить 14) — саме тому коротка
     * відповідь від micom лишається у FIFO, і про неї повідомляє DR.
     */
    int i;

    for (i = 0; i < size && s->fifo_len < QY8_SCIF_FIFO; i++) {
        s->fifo[s->fifo_len++] = buf[i];
    }
    s->dr = false;
    qy8_scif_rx_pump(s);
    timer_mod(s->idle, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                       QY8_SCIF_IDLE_NS);
}

/* --- TMU (таймери Renesas, регістрова мапа як у SH TMU) --------------- */

#define QY8_TMU_BASE        0xFFD80000
#define QY8_TMU_CHANS       3
#define QY8_TMU_SPI0        32          /* канал n -> SPI 32+n -> IRQ 64+n;
                                           перевірено: eboot вмикає IRQ 65
                                           одночасно зі стартом каналу 1 */
#define QY8_PCLK            65000000    /* P-clock R8A7778 */

#define TMU_TOCR    0x00
#define TMU_TSTR    0x04
#define TMU_CH(n)   (0x08 + (n) * 0x0c)   /* TCOR, +4 TCNT, +8 TCR */

#define TCR_TPSC    0x0007
#define TCR_UNIE    0x0020
#define TCR_UNF     0x0100

typedef struct Qy8TmuChan {
    ptimer_state *ptimer;
    qemu_irq irq;
    uint32_t tcor;
    uint16_t tcr;
    bool running;
} Qy8TmuChan;

typedef struct Qy8Tmu {
    MemoryRegion mr;
    Qy8TmuChan ch[QY8_TMU_CHANS];
    uint8_t tstr;
    uint8_t tocr;
} Qy8Tmu;

static void qy8_tmu_chan_update_irq(Qy8TmuChan *c)
{
    qemu_set_irq(c->irq, (c->tcr & TCR_UNF) && (c->tcr & TCR_UNIE));
}

static void qy8_tmu_tick(void *opaque)
{
    Qy8TmuChan *c = opaque;

    c->tcr |= TCR_UNF;
    qy8_tmu_chan_update_irq(c);
}

static uint32_t qy8_tmu_freq(const Qy8TmuChan *c)
{
    /* TPSC: 0=P/4 1=P/16 2=P/64 3=P/256 4=P/1024 */
    static const uint32_t div[8] = { 4, 16, 64, 256, 1024, 1024, 1024, 1024 };
    return QY8_PCLK / div[c->tcr & TCR_TPSC];
}

static void qy8_tmu_chan_restart(Qy8TmuChan *c, bool run)
{
    ptimer_transaction_begin(c->ptimer);
    if (run) {
        ptimer_set_freq(c->ptimer, qy8_tmu_freq(c));
        ptimer_set_limit(c->ptimer, c->tcor ? c->tcor : 1, 0);
        ptimer_run(c->ptimer, 0);
    } else {
        ptimer_stop(c->ptimer);
    }
    ptimer_transaction_commit(c->ptimer);
    c->running = run;
}

static uint64_t qy8_tmu_read(void *opaque, hwaddr addr, unsigned size)
{
    Qy8Tmu *t = opaque;
    int n;

    if (addr == TMU_TOCR) {
        return t->tocr;
    }
    if (addr == TMU_TSTR) {
        return t->tstr;
    }
    for (n = 0; n < QY8_TMU_CHANS; n++) {
        hwaddr b = TMU_CH(n);
        if (addr == b) {
            return t->ch[n].tcor;
        }
        if (addr == b + 4) {
            return ptimer_get_count(t->ch[n].ptimer);
        }
        if (addr == b + 8) {
            return t->ch[n].tcr;
        }
    }
    return 0;
}

static void qy8_tmu_write(void *opaque, hwaddr addr, uint64_t val,
                          unsigned size)
{
    Qy8Tmu *t = opaque;
    int n;

    if (addr == TMU_TOCR) {
        t->tocr = val;
        return;
    }
    if (addr == TMU_TSTR) {
        t->tstr = val;
        for (n = 0; n < QY8_TMU_CHANS; n++) {
            bool run = (val >> n) & 1;
            if (run != t->ch[n].running) {
                qy8_tmu_chan_restart(&t->ch[n], run);
            }
        }
        return;
    }
    for (n = 0; n < QY8_TMU_CHANS; n++) {
        Qy8TmuChan *c = &t->ch[n];
        hwaddr b = TMU_CH(n);

        if (addr == b) {
            c->tcor = val;
            ptimer_transaction_begin(c->ptimer);
            ptimer_set_limit(c->ptimer, val ? val : 1, 0);
            ptimer_transaction_commit(c->ptimer);
            return;
        }
        if (addr == b + 4) {
            ptimer_transaction_begin(c->ptimer);
            ptimer_set_count(c->ptimer, val);
            ptimer_transaction_commit(c->ptimer);
            return;
        }
        if (addr == b + 8) {
            /* UNF скидається записом нуля в цей біт */
            c->tcr = (val & ~TCR_UNF) | (c->tcr & val & TCR_UNF);
            if (c->running) {
                ptimer_transaction_begin(c->ptimer);
                ptimer_set_freq(c->ptimer, qy8_tmu_freq(c));
                ptimer_transaction_commit(c->ptimer);
            }
            qy8_tmu_chan_update_irq(c);
            return;
        }
    }
}

static const MemoryRegionOps qy8_tmu_ops = {
    .read = qy8_tmu_read,
    .write = qy8_tmu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

/* --- контролер плати (зовнішня мікросхема на CS, 16-бітні регістри) ---- */

/*
 * Це не блок SoC, а окремий чип на шині CS; у прошивці він — джерело DIPSW
 * і сигналів дозволу старту. Що саме з нього читають, знято з коду:
 *
 *   +0x00  статус. Завантажувач у циклі @0x1148 читає його двічі поспіль
 *          (антидребезг зовнішньої шини) і дивиться біт 0x400 — «можна
 *          виходити зі standby»; без нього лічильник r7 не спадає до нуля
 *          і машина йде на скидання по watchdog @0x11e0. Біт 0x80 керує
 *          другим лічильником (r6), за яким @0x1a6c у +0x04 пишеться
 *          0x8000 або 0.
 *   +0x02  DIPSW. Молодші 3 біти: перша стадія @0x12c4 порівнює з 4
 *          (режим оновлення з флеш-офсета 0x60000), а eboot друкує
 *          "DIPSW=%x" @0x97c0686c і обирає за ними режим буту.
 *   +0x04, +0x06, +0x08, +0x0c, +0x14 — пишуться під час ініціалізації.
 */
typedef struct Qy8Bctl {
    MemoryRegion mr;
    uint16_t reg[QY8_BCTL_SIZE / 2];
} Qy8Bctl;

static uint64_t qy8_bctl_read(void *opaque, hwaddr addr, unsigned size)
{
    Qy8Bctl *b = opaque;

    return b->reg[addr >> 1];
}

static void qy8_bctl_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned size)
{
    Qy8Bctl *b = opaque;

    /*
     * +0x00 при читанні — статус, при записі — щось інше: перша стадія
     * @0x1a6c кладе туди 0x2000 і одразу після того читає той самий
     * регістр, очікуючи цілі біти 0x80/0x400. Якщо дати запису затерти
     * читану половину, наступний тік нагляду @0x97c10e84 бачить 0x400
     * скинутим, ставить стан 1, а ще через тік іде на скидання по
     * watchdog @0x97c10d38 ("_p"). Тобто плани читання й запису тут
     * різні — запис у статус просто не чіпає те, що читається.
     */
    if (addr == BCTL_STATUS) {
        return;
    }
    b->reg[addr >> 1] = val;
}

static const MemoryRegionOps qy8_bctl_ops = {
    .read = qy8_bctl_read,
    .write = qy8_bctl_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 2,
    .impl.max_access_size = 2,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};



/* --- простір, який ні в що не декодується ----------------------------- */

/*
 * QEMU за замовчуванням відповідає на доступ до нічийної фізичної адреси
 * зовнішнім аварійним завершенням (data/prefetch abort). Ця плата так не
 * поводиться, і доказ тому — сама прошивка: ядро WinCE ЗОНДУЄ пам'ять.
 *
 * Процедура @0x880489ec отримує (початок, довжина, робоча VA), відображає
 * сторінку-кандидат у вікно 0xC04B0000 і двійковим пошуком шукає верхню межу
 * ОЗП: зберігає два слова, пише підпис 0x6a08bc95/0xfd1247e3, читає назад і
 * за збігом рухає нижню межу вгору, а за розбіжністю — верхню вниз.
 *
 * Перший виклик іде по справжньому ОЗП (PA 0x0d6f0000, 0x9e0000 — це
 * ulRAMStart..ulRAMEnd із ROMHDR ядра) і проходить. Другий питає про
 * можливий додатковий банк PA 0x44000000 розміром 64 МБ — адреси, якої
 * немає в жодному відображенні самого ядра (його таблиця L1: флеш @0,
 * DDR @0x08000000 256 МБ, вікна 0x18000000, 0xf0000000, 0xfc000000).
 * Тобто зондування нічийного простору тут — штатний хід, і читання звідти
 * мусить просто повернути сміття, інакше алгоритм узагалі не міг би
 * працювати. У нас же воно давало abort -> ядро падало у вектор @0xffff0010.
 *
 * Тому нижнім шаром кладемо «порожнечу»: читання дає нулі, запис гине.
 * Пріоритет нижчий за все інше, зокрема за qy8.periph (-1000), тож жоден
 * справжній регістр вона не перехоплює, а доступи все одно видно з -d unimp.
 */
static uint64_t qy8_void_read(void *opaque, hwaddr addr, unsigned size)
{
    qemu_log_mask(LOG_UNIMP, "qy8.void: читання з нічийної адреси %#" HWADDR_PRIx
                  " (%u Б)\n", addr, size);
    return 0;
}

static void qy8_void_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned size)
{
    qemu_log_mask(LOG_UNIMP, "qy8.void: запис у нічийну адресу %#" HWADDR_PRIx
                  " = %#" PRIx64 "\n", addr, val);
}

static const MemoryRegionOps qy8_void_ops = {
    .read = qy8_void_read,
    .write = qy8_void_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
};

/* --- USB-PHY (R-Car Gen1) @0xFFE70800 --------------------------------- */


/*
 * Перший блок, на якому спіткнулося саме ЯДРО, а не завантажувач.
 *
 * Регістрова мапа взята з Linux (`drivers/usb/phy/phy-rcar-usb.c`,
 * сумісність "renesas,usb-phy-r8a7778"), а не вгадана; там же вузол
 * usb-phy@ffe70800 має другим вікном 0xffe76000.
 *
 *   +0x00 USBPCTRL0
 *   +0x04 USBPCTRL1 — біт0 PHY_ENB, біт1 PLL_ENB, біт2 PHY_RST
 *   +0x08 USBST     — тільки читання: біт31 ST_ACT, біт30 ST_PLL
 *
 * Прошивка робить рівно те, що й драйвер Linux: пише в USBPCTRL1 спершу
 * 1 (PHY_ENB), потім 3 (PHY_ENB|PLL_ENB) і чекає, поки в USBST стануть
 * обидва біти — «PLL захопився». Поки регістр читався нулем, ядро
 * крутилося в цьому опитуванні вічно (5,3 млн читань за 20 с у `-d unimp`).
 */
#define QY8_USBPHY_BASE     0xFFE70800
#define QY8_USBPHY_SIZE     0x100

#define USBPCTRL1           0x04
#define USBST               0x08

#define USBPCTRL1_PHY_ENB   (1u << 0)
#define USBPCTRL1_PLL_ENB   (1u << 1)
#define USBST_ACT           (1u << 31)
#define USBST_PLL           (1u << 30)

typedef struct Qy8UsbPhy {
    MemoryRegion mr;
    uint32_t reg[QY8_USBPHY_SIZE / 4];
} Qy8UsbPhy;

static uint64_t qy8_usbphy_read(void *opaque, hwaddr addr, unsigned size)
{
    Qy8UsbPhy *u = opaque;
    uint32_t ctrl1 = u->reg[USBPCTRL1 / 4];

    if (addr == USBST) {
        /* PLL «захоплюється» миттєво, щойно ввімкнено PHY і PLL */
        if ((ctrl1 & (USBPCTRL1_PHY_ENB | USBPCTRL1_PLL_ENB)) ==
            (USBPCTRL1_PHY_ENB | USBPCTRL1_PLL_ENB)) {
            return USBST_ACT | USBST_PLL;
        }
        return 0;
    }
    return u->reg[addr / 4];
}

static void qy8_usbphy_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    Qy8UsbPhy *u = opaque;

    if (addr == USBST) {          /* статус — тільки читання */
        return;
    }
    u->reg[addr / 4] = val;
}

static const MemoryRegionOps qy8_usbphy_ops = {
    .read = qy8_usbphy_read,
    .write = qy8_usbphy_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

/* --- GPIO (R-Car, 6 банків по 0x1000) --------------------------------- */


#define QY8_GPIO_BASE       0xFFC40000
#define QY8_GPIO_BANKS      6
#define QY8_GPIO_STRIDE     0x1000

#define GPIO_IOINTSEL   0x00
#define GPIO_INOUTSEL   0x04
#define GPIO_OUTDT      0x08
#define GPIO_INDT       0x0c
#define GPIO_INTDT      0x10
#define GPIO_INTCLR     0x14
#define GPIO_INTMSK     0x18
#define GPIO_MSKCLR     0x1c
#define GPIO_POSNEG     0x20
#define GPIO_EDGLEVEL   0x24
#define GPIO_FILONOFF   0x28

typedef struct Qy8Gpio {
    MemoryRegion mr;
    uint32_t iointsel, inoutsel, outdt;
    uint32_t intmsk, posneg, edglevel, filonoff;
    uint32_t in_level;          /* рівні на вхідних лініях */
    int bank;
} Qy8Gpio;

static uint64_t qy8_gpio_read(void *opaque, hwaddr addr, unsigned size)
{
    Qy8Gpio *g = opaque;

    switch (addr) {
    case GPIO_IOINTSEL:
        return g->iointsel;
    case GPIO_INOUTSEL:
        return g->inoutsel;
    case GPIO_OUTDT:
        return g->outdt;
    case GPIO_INDT:
        /* виходи читаються як те, що ми туди записали; входи — рівень лінії */
        return (g->outdt & g->inoutsel) | (g->in_level & ~g->inoutsel);
    case GPIO_INTMSK:
        return g->intmsk;
    case GPIO_POSNEG:
        return g->posneg;
    case GPIO_EDGLEVEL:
        return g->edglevel;
    case GPIO_FILONOFF:
        return g->filonoff;
    default:
        return 0;
    }
}

static void qy8_gpio_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned size)
{
    Qy8Gpio *g = opaque;

    switch (addr) {
    case GPIO_IOINTSEL:
        g->iointsel = val;
        break;
    case GPIO_INOUTSEL:
        g->inoutsel = val;
        break;
    case GPIO_OUTDT:
        g->outdt = val;
        break;
    case GPIO_INTMSK:
        g->intmsk = val;
        break;
    case GPIO_MSKCLR:
        g->intmsk &= ~val;
        break;
    case GPIO_POSNEG:
        g->posneg = val;
        break;
    case GPIO_EDGLEVEL:
        g->edglevel = val;
        break;
    case GPIO_FILONOFF:
        g->filonoff = val;
        break;
    default:
        break;
    }
}

static const MemoryRegionOps qy8_gpio_ops = {
    .read = qy8_gpio_read,
    .write = qy8_gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

/*
 * Рівні на вхідних лініях плати.
 *
 * GPIO1 біт 0 — сигнал «лишатися в standby». Цикл @0x1148 читає INDT двічі
 * поспіль і, якщо біт стоїть, іде на 0x1188: там r8:=1, і коли лічильник
 * r7 добігає нуля, @0x120c перевіряє r8 і заводить watchdog @0x11e0 на
 * скидання. Тобто одиниця на цьому піні — це не «плата готова», а «не
 * вантажитися»; щоб бут пішов далі, лінія має бути в нулі. Той самий пін
 * @0x1bfc отримує GPIO1.INTCLR біт 0, тобто він ще й джерело переривання.
 */
static const uint32_t qy8_gpio_in_level[QY8_GPIO_BANKS] = {
    [1] = 0x00000000,
};

/* --- машина ----------------------------------------------------------- */

#define TYPE_QY8_MACHINE MACHINE_TYPE_NAME("clarion-qy8")
OBJECT_DECLARE_SIMPLE_TYPE(Qy8MachineState, QY8_MACHINE)

struct Qy8MachineState {
    MachineState parent;

    ARMCPU *cpu;
    DeviceState *gic;
    DeviceState *du;
    DeviceState *dmac;
    Qy8Scif scif[QY8_NUM_SCIF];
    Qy8Tmu tmu;
    Qy8Gpio gpio[QY8_GPIO_BANKS];
    Qy8Bctl bctl;
    Qy8UsbPhy usbphy;
    MemoryRegion voidmr;

    uint8_t dipsw;              /* режим буту, властивість машини */

    MemoryRegion flash;
    MemoryRegion ddr0, ddr1;
    MemoryRegion sram0, sram1;
    MemoryRegion ddr1_shadow;
};

static void qy8_add_ram(MemoryRegion *sysmem, MemoryRegion *mr,
                        const char *name, hwaddr base, uint64_t size)
{
    memory_region_init_ram(mr, NULL, name, size, &error_fatal);
    memory_region_add_subregion(sysmem, base, mr);
}

static void qy8_init(MachineState *machine)
{
    Qy8MachineState *s = QY8_MACHINE(machine);
    MemoryRegion *sysmem = get_system_memory();
    SysBusDevice *gicbusdev;
    char *fname;
    ssize_t sz;
    int i;

    s->cpu = ARM_CPU(object_new(machine->cpu_type));
    object_property_set_bool(OBJECT(s->cpu), "has_el3", false, &error_fatal);
    /* скидання починається з 0x0 — флеш CS0, XIP */
    object_property_set_int(OBJECT(s->cpu), "rvbar", 0, NULL);
    qdev_realize(DEVICE(s->cpu), NULL, &error_fatal);

    /* --- пам'ять --- */

    /* найнижчий шар: нічийні адреси читаються нулями, а не дають abort */
    memory_region_init_io(&s->voidmr, NULL, &qy8_void_ops, s,
                          "qy8.void", 0x100000000ULL);
    memory_region_add_subregion_overlap(sysmem, 0, &s->voidmr, -1500);

    memory_region_init_rom(&s->flash, NULL, "qy8.flash",
                           QY8_FLASH_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, QY8_FLASH_BASE, &s->flash);

    qy8_add_ram(sysmem, &s->ddr0, "qy8.ddr0", QY8_DDR0_BASE, QY8_DDR0_SIZE);
    qy8_add_ram(sysmem, &s->ddr1, "qy8.ddr1", QY8_DDR1_BASE, QY8_DDR1_SIZE);
    qy8_add_ram(sysmem, &s->sram0, "qy8.sram0", QY8_SRAM0_BASE, QY8_SRAM0_SIZE);
    qy8_add_ram(sysmem, &s->sram1, "qy8.sram1", QY8_SRAM1_BASE, QY8_SRAM1_SIZE);

    /*
     * Тінь DDR1 за PA 0x90000000 — милиця під конвеєр, а не деталь плати.
     *
     * `Launch` завантажувача (VA 0x97c1204c) вимикає MMU і ОДРАЗУ наступною
     * інструкцією стрибає на фізичну адресу трампліна:
     *
     *      97c12118  mcr p15,0,r1,c1,c0,0   ; SCTLR.M := 0 — MMU вимкнено
     *      97c1211c  mov pc, r0             ; r0 = VAtoPA(0x97c12130) = 0x17c12130
     *      97c12120  nop x4                 ; набивка під злив конвеєра
     *
     * На живому Cortex-A9 `mov pc,r0` уже вибрано в конвеєр, поки MMU був
     * увімкнений, тож вибірка за старим відображенням; ARM ARM і не обіцяє,
     * що зміна SCTLR.M побачиться до context-synchronizing operation. QEMU ж
     * застосовує запис миттєво: обриває блок трансляції й перевибирає
     * 0x97c1211c уже з вимкненим MMU, тобто за PA 0x97C1211C. Там у нас нічого
     * немає -> prefetch abort -> PC=0xc -> «нульовий слайд» по таблиці векторів
     * до ASCII-імені @0x28 -> data abort -> вічна петля. Саме це й ловив
     * `info registers` (abt32, PC=0x10).
     *
     * Тому робимо ті кілька слів досяжними за фізичною адресою, рівною їхній
     * VA: DDR1 (PA 0x10000000, там живе eboot) ще раз видно за 0x90000000 —
     * рівно так, як його ж OEMAddressTable ставить VA 0x90000000 -> PA
     * 0x10000000. Це вікно потрібне тільки на час «MMU вже вимкнено, а PC ще
     * віртуальний»; далі виконання йде за справжніми PA (0x17c12130, потім
     * ядро @0x08001000).
     */
    memory_region_init_alias(&s->ddr1_shadow, NULL, "qy8.ddr1-shadow",
                             &s->ddr1, 0, QY8_DDR1_SIZE);
    memory_region_add_subregion(sysmem, 0x90000000, &s->ddr1_shadow);

    /* --- вміст флеш: дамп плати через -bios --- */
    if (!machine->firmware) {
        error_report("clarion-qy8: треба -bios <дамп флеш 64 МБ>");
        exit(1);
    }
    fname = g_strdup(machine->firmware);
    sz = load_image_mr(fname, &s->flash);
    if (sz < 0) {
        error_report("clarion-qy8: не вдалося прочитати %s", fname);
        exit(1);
    }
    /*
     * Годиться будь-який дамп CS0 — стоковий, чужий чи власноруч
     * патчений: машина нічого з нього не розбирає, вона просто кладе
     * байти під reset-вектор. Розмір має бути рівно 64 МБ (дамп без OOB),
     * інакше решта вікна лишиться нулями і бут піде не туди.
     */
    if (sz != QY8_FLASH_SIZE) {
        warn_report("clarion-qy8: %s має %d байт, а не 64 МБ — "
                    "решта флеш-вікна лишиться нулями", fname, (int)sz);
    }
    g_free(fname);

    /* --- GIC (в A9 R-Car Gen1 він окремий, не в private region) --- */
    s->gic = qdev_new(TYPE_ARM_GIC);
    qdev_prop_set_uint32(s->gic, "revision", 2);
    qdev_prop_set_uint32(s->gic, "num-cpu", 1);
    qdev_prop_set_uint32(s->gic, "num-irq", QY8_NUM_IRQ);
    gicbusdev = SYS_BUS_DEVICE(s->gic);
    sysbus_realize_and_unref(gicbusdev, &error_fatal);
    sysbus_mmio_map(gicbusdev, 0, QY8_GIC_DIST_BASE);
    sysbus_mmio_map(gicbusdev, 1, QY8_GIC_CPU_BASE);
    sysbus_connect_irq(gicbusdev, 0,
                       qdev_get_gpio_in(DEVICE(s->cpu), ARM_CPU_IRQ));
    sysbus_connect_irq(gicbusdev, 1,
                       qdev_get_gpio_in(DEVICE(s->cpu), ARM_CPU_FIQ));

    /*
     * --- HPB-DMAC ---
     *
     * Бази й переривання — з hpb_dmae_resources[] у setup-r8a7778.c:
     * канальні регістри 0xffc08000/0x1000, спільні 0xffc09000/0x170,
     * gic_iid(0x7b) і 5 ліній поспіль (IRQ 123..127 = SPI 91..95).
     * Ставимо до SCIF, бо приймачі SCIF віддають байти саме сюди.
     */
    s->dmac = qdev_new(TYPE_CLARION_HPBDMA);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->dmac), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->dmac), 0, CLARION_HPBDMA_CHAN_BASE);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->dmac), 1, CLARION_HPBDMA_COMM_BASE);
    for (i = 0; i < CLARION_HPBDMA_NUM_IRQ; i++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(s->dmac), i,
                           qdev_get_gpio_in(s->gic,
                                            CLARION_HPBDMA_IRQ_BASE_SPI + i));
    }

    /* --- SCIF --- */
    for (i = 0; i < QY8_NUM_SCIF; i++) {
        Qy8Scif *sc = &s->scif[i];
        char *name = g_strdup_printf("qy8.scif%d", i);

        sc->index = i;
        sc->base = QY8_SCIF_BASE + i * QY8_SCIF_STRIDE;
        sc->idle = timer_new_ns(QEMU_CLOCK_VIRTUAL, qy8_scif_idle_expire, sc);
        sc->dmac = s->dmac;
        sc->irq = qdev_get_gpio_in(s->gic, QY8_SCIF_SPI0 + i);
        memory_region_init_io(&sc->mr, NULL, &qy8_scif_ops, sc, name, 0x100);
        memory_region_add_subregion(sysmem,
                                    QY8_SCIF_BASE + i * QY8_SCIF_STRIDE,
                                    &sc->mr);
        if (serial_hd(qy8_scif_chr_index(i))) {
            qemu_chr_fe_init(&sc->chr, serial_hd(qy8_scif_chr_index(i)),
                             &error_abort);
            qemu_chr_fe_set_handlers(&sc->chr, qy8_scif_can_receive,
                                     qy8_scif_receive, NULL, NULL,
                                     sc, NULL, true);
        }
        g_free(name);
    }

    /* --- TMU --- */
    for (i = 0; i < QY8_TMU_CHANS; i++) {
        Qy8TmuChan *c = &s->tmu.ch[i];

        c->irq = qdev_get_gpio_in(s->gic, QY8_TMU_SPI0 + i);
        c->ptimer = ptimer_init(qy8_tmu_tick, c, PTIMER_POLICY_LEGACY);
        c->tcor = 0xffffffff;
    }
    memory_region_init_io(&s->tmu.mr, NULL, &qy8_tmu_ops, &s->tmu,
                          "qy8.tmu", 0x30);
    memory_region_add_subregion(sysmem, QY8_TMU_BASE, &s->tmu.mr);

    /* --- GPIO --- */
    for (i = 0; i < QY8_GPIO_BANKS; i++) {
        Qy8Gpio *g = &s->gpio[i];
        char *name = g_strdup_printf("qy8.gpio%d", i);

        g->bank = i;
        g->in_level = qy8_gpio_in_level[i];
        memory_region_init_io(&g->mr, NULL, &qy8_gpio_ops, g, name, 0x1000);
        memory_region_add_subregion(sysmem,
                                    QY8_GPIO_BASE + i * QY8_GPIO_STRIDE,
                                    &g->mr);
        g_free(name);
    }

    /* --- контролер плати: DIPSW і дозвіл виходу зі standby --- */
    s->bctl.reg[BCTL_STATUS >> 1] = BCTL_ST_PWR | BCTL_ST_BOOT;
    s->bctl.reg[BCTL_DIPSW >> 1] = s->dipsw & 7;
    memory_region_init_io(&s->bctl.mr, NULL, &qy8_bctl_ops, &s->bctl,
                          "qy8.bctl", QY8_BCTL_SIZE);
    /* перекриває вікно sram1, бо на платі це той самий CS */
    memory_region_add_subregion_overlap(sysmem, QY8_BCTL_BASE,
                                        &s->bctl.mr, 1);

    /* USB-PHY: перекриває широке вікно qy8.periph (див. нижче) */
    memory_region_init_io(&s->usbphy.mr, NULL, &qy8_usbphy_ops, &s->usbphy,
                          "qy8.usbphy", QY8_USBPHY_SIZE);
    memory_region_add_subregion_overlap(sysmem, QY8_USBPHY_BASE,
                                        &s->usbphy.mr, 1);

    /* --- Display Unit: справжнє вікно QEMU --- */
    /*
     * Лінію переривання (GIC_SPI 31) модель поки не піднімає: щоб рахувати
     * кадри, треба точкова частота, а вона задається поза DU (ESCR02 тут 0,
     * тобто такт зовнішній). Джерела на неї немає — тож не вигадуємо.
     */
    s->du = qdev_new(TYPE_CLARION_DU);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->du), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->du), 0, QY8_DU_BASE);

    /* --- решта периферії: поки лише лог доступів (-d unimp) --- */
    create_unimplemented_device("qy8.cs1",   QY8_CS1_BASE, QY8_CS1_SIZE);
    create_unimplemented_device("qy8.l2c",   QY8_L2C_BASE, 0x1000);
    create_unimplemented_device("qy8.dbsc3", 0xFE800000, 0x10000);  /* DDR */
    create_unimplemented_device("qy8.intc2", 0xFE780000, 0x1000);
    create_unimplemented_device("qy8.cpg",   0xFFC80000, 0x1000);
    create_unimplemented_device("qy8.pfc",   0xFFFC0000, 0x1000);
    create_unimplemented_device("qy8.rst",   0xFFCC0000, 0x1000);
    /* широкий перехоплювач: усе інше згори 0xF0000000 логується */
    create_unimplemented_device("qy8.periph", 0xF0000000, 0x10000000);
}

static void qy8_machine_instance_init(Object *obj)
{
    Qy8MachineState *s = QY8_MACHINE(obj);

    /*
     * За замовчуванням 5 = "NORM(RES)>>" — звичайний бут із повним
     * налагоджувальним виводом; на живій платі це те саме, що заземлити
     * TEST_B1 (docs/16). Стокове значення непаяної плати — 7 (тихий бут).
     */
    s->dipsw = QY8_DIPSW_NORM_RES;
    object_property_add_uint8_ptr(obj, "dipsw", &s->dipsw,
                                  OBJ_PROP_FLAG_READWRITE);
    object_property_set_description(obj, "dipsw",
        "режим буту з DIPSW контролера плати, 0..7 (5 = NORM(RES) з логом)");
}

static void qy8_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    static const char * const valid_cpu_types[] = {
        ARM_CPU_TYPE_NAME("cortex-a9"),
        NULL
    };

    mc->desc = "Clarion QY8XXX head unit (Renesas R8A7778, WinCE)";
    mc->init = qy8_init;
    mc->min_cpus = 1;
    mc->max_cpus = 1;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-a9");
    mc->valid_cpu_types = valid_cpu_types;
    mc->default_ram_size = QY8_DDR0_SIZE + QY8_DDR1_SIZE;
    mc->no_floppy = 1;
    mc->no_cdrom = 1;
    mc->no_parallel = 1;
    /* RAM машина створює сама — за картою з OEMAddressTable */
    mc->default_ram_id = NULL;
}

static const TypeInfo qy8_machine_types[] = {
    {
        .name           = TYPE_QY8_MACHINE,
        .parent         = TYPE_MACHINE,
        .instance_size  = sizeof(Qy8MachineState),
        .instance_init  = qy8_machine_instance_init,
        .class_init     = qy8_machine_class_init,
    },
};

DEFINE_TYPES(qy8_machine_types)
