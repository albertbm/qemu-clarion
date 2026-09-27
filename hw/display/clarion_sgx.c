/*
 * PowerVR SGX плати Clarion QY8XXX (Nissan Leaf ZE1, WinCE 7.0) —
 * пасивна модель MMIO і трасувальник BIF/MMU.
 *
 * Навіщо. Графічний стек прошивки впирається саме тут: `AuiApp` ->
 * `eglGetDisplay` -> `services_init` -> `SGXInitialise` пише 1 у
 * `EUR_CR_EVENT_KICK2` і опитує `SGXMKIF_HOST_CTL.ui32InitStatus`, чекаючи
 * біта `PVRSRV_USSE_EDM_INIT_COMPLETE`. Мікроядра в моделі немає, біт не
 * з'являється, через таймаут виходить `PVRSRV_ERROR_RETRY`, `services_init`
 * падає і EGL віддає `EGL_NO_DISPLAY` (docs/28-sgx-roadmap.md).
 *
 * Що ЦЯ модель робить і чого свідомо НЕ робить.
 *
 * Робить: віддає блоку власне вікно замість широкого перехоплювача плати,
 * запам'ятовує все, що гість записав, і на kick друкує розбір стану —
 * включно з повним обходом каталогу сторінок SGX і вмістом об'єкта, на який
 * показує пара «база + зсув» із регістрів. Тобто перетворює блок із чорної
 * скриньки на прилад.
 *
 * НЕ робить: не виставляє `ui32InitStatus`, не піднімає переривань, не
 * повертає нічого, крім нуля, на читання. Отже для гостя її поява —
 * поведінково НІЩО: він бачить рівно те саме, що бачив від
 * `unimplemented-device` (той теж віддавав нулі), і так само доходить до
 * таймауту. Це навмисно: щабель M1 карти має бути перевірений A/B-прогоном
 * «бут не змінився», і лише потім можна обговорювати M3.
 *
 * Чому не виставляємо `ui32InitStatus` уже зараз. Бо честь моделі важливіша
 * за проходження перевірки. Адреса блоку `SGXMKIF_HOST_CTL` не приходить у
 * жодному регістрі. Доведений ланцюг такий:
 *
 *   EUR_CR_PDS_EXEC_BASE (0x0AB8) = 0x0DC00000          <- MMIO
 *   невідомий регістр    (0x0A68) = 0x0080C180          <- MMIO
 *   сума                          = 0x0E40C180           = алокація SGX
 *   ???                                                  <- ЄДИНА ДІРА
 *   об'єкт @0x0E40C1B0 +0x00      = 0x0F003000           = контейнер
 *   контейнер            +0x04    = 0x0F003120           = HOST_CTL
 *   HOST_CTL             +0x00    = ui32InitStatus
 *
 * Доки діра відкрита, будь-яка реалізація M3 означала б або зашиту адресу
 * цього образу, або сканування пам'яті в пошуках підпису — і те, і те
 * заборонено контрактом проєкту. Повний розбір і критерії — у
 * docs/sgx/09-edm-boot-locator.md репозиторію nissan-can-explore.
 *
 * Звідки взято базу, розмір, зсуви й маски — див. clarion_sgx.h; жодне
 * число тут не вгадане з аналогії з іншим SoC.
 *
 * Перемикачі середовища (усі за замовчуванням вимкнені, крім звіту):
 *
 *   QY8_SGX=off              прибрати модель зовсім (A/B проти перехоплювача)
 *   QY8_SGX_KICKS=N          скільки перших kick'ів розбирати докладно (2)
 *   QY8_SGX_READBACK=1       ⚠ віддавати на читання те, що було записано.
 *                            ЗМІНЮЄ видиму гостем поведінку — тільки для
 *                            досліду, не для звичайних прогонів.
 *   QY8_SGX_DUMP=VA:LEN,...  додатково показувати ці діапазони GPU-VA на kick
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/sysbus.h"
#include "hw/display/clarion_sgx.h"
#include "system/address-spaces.h"
#include "qom/object.h"
#include "trace.h"

#define SGX_NREGS           (CLARION_SGX_SIZE / 4)
#define SGX_DUMP_RANGES     8
#define SGX_WR_JOURNAL      512
#define SGX_FIND_TARGETS    8

typedef struct ClarionSgxDump {
    uint32_t va;
    uint32_t len;
} ClarionSgxDump;

typedef struct ClarionSgxWrite {
    uint32_t off;
    uint32_t val;
} ClarionSgxWrite;

struct ClarionSgxState {
    SysBusDevice parent_obj;

    MemoryRegion mr;

    /* Усе, що гість записав. Ніщо тут не має власної семантики. */
    uint32_t regs[SGX_NREGS];

    uint32_t kicks;             /* скільки разів прийшов EVENT_KICK2 */
    uint32_t kick_reports;      /* скільки з них розбирати докладно */
    bool readback;              /* ⚠ віддавати записане на читання */

    ClarionSgxDump dump[SGX_DUMP_RANGES];
    unsigned ndump;

    /* Журнал записів у порядку надходження — щоб бачити й ті, яких немає
     * в init-script'і (наприклад BIF_DIR_LIST_BASE0 пише сам SGXReset). */
    ClarionSgxWrite wr[SGX_WR_JOURNAL];
    unsigned nwr;
    bool wr_overflow;
    bool show_writes;

    uint32_t find[SGX_FIND_TARGETS];   /* QY8_SGX_FIND */
    unsigned nfind;
    bool graph;                        /* QY8_SGX_GRAPH */
};
typedef struct ClarionSgxState ClarionSgxState;

OBJECT_DECLARE_SIMPLE_TYPE(ClarionSgxState, CLARION_SGX)

static uint32_t sgx_reg(ClarionSgxState *s, hwaddr off)
{
    return s->regs[off / 4];
}

/* --- обхід MMU SGX ---------------------------------------------------- */

/*
 * Прочитати слово з ФІЗИЧНОЇ пам'яті гостя. Саме з пам'яті, а не з нашого
 * дзеркала регістрів: інакше звіт підтверджував би сам себе.
 */
static uint32_t sgx_phys_ld32(uint32_t pa)
{
    uint32_t v = 0;

    address_space_read(&address_space_memory, pa, MEMTXATTRS_UNSPECIFIED,
                       &v, sizeof(v));
    return le32_to_cpu(v);
}

/*
 * Перекласти device-VA у фізичну адресу за каталогом pd_pa.
 * Повертає false, якщо PDE або PTE невалідний — і тоді НІЧОГО не вигадує.
 */
static bool sgx_translate(uint32_t pd_pa, uint32_t va, uint32_t *pa_out)
{
    uint32_t pde = sgx_phys_ld32(pd_pa + 4 * (va >> SGX_MMU_PD_SHIFT));
    uint32_t pt_pa, pte, idx;

    if (!(pde & SGX_MMU_ENTRY_VALID)) {
        return false;
    }
    pt_pa = pde & SGX_MMU_ENTRY_ADDR_MASK;
    idx = (va >> SGX_MMU_PAGE_SHIFT) & (SGX_MMU_ENTRIES - 1);
    pte = sgx_phys_ld32(pt_pa + 4 * idx);
    if (!(pte & SGX_MMU_ENTRY_VALID)) {
        return false;
    }
    *pa_out = (pte & SGX_MMU_ENTRY_ADDR_MASK) | (va & (SGX_MMU_PAGE_SIZE - 1));
    return true;
}

/* Перелік валідних PDE з кількістю відображених сторінок у кожному. */
static void sgx_report_pd(uint32_t pd_pa)
{
    unsigned i, total = 0, ndir = 0;

    for (i = 0; i < SGX_MMU_ENTRIES; i++) {
        uint32_t pde = sgx_phys_ld32(pd_pa + 4 * i);
        uint32_t pt_pa;
        unsigned j, n = 0;

        if (!(pde & SGX_MMU_ENTRY_VALID)) {
            continue;
        }
        pt_pa = pde & SGX_MMU_ENTRY_ADDR_MASK;
        for (j = 0; j < SGX_MMU_ENTRIES; j++) {
            if (sgx_phys_ld32(pt_pa + 4 * j) & SGX_MMU_ENTRY_VALID) {
                n++;
            }
        }
        ndir++;
        total += n;
        fprintf(stderr, "[sgx]   PDE[%4u] = %08x  таблиця PA %08x  "
                "VA %08x..%08x  сторінок %u\n",
                i, pde, pt_pa, i << SGX_MMU_PD_SHIFT,
                (i << SGX_MMU_PD_SHIFT) + 0x3FFFFF, n);
    }
    fprintf(stderr, "[sgx]   валідних PDE %u, відображених сторінок %u\n",
            ndir, total);
}

/* Показати діапазон GPU-VA. len обрізаємо, щоб звіт лишався звітом. */
static void sgx_report_range(uint32_t pd_pa, const char *what,
                             uint32_t va, uint32_t len)
{
    uint32_t off;

    len = MIN(len, 0x200);
    fprintf(stderr, "[sgx]   %s GPU VA %08x, %u Б:\n", what, va, len);
    for (off = 0; off < len; off += 16) {
        uint32_t pa, w[4];
        unsigned k, n = MIN(4, (len - off + 3) / 4);

        if (!sgx_translate(pd_pa, va + off, &pa)) {
            fprintf(stderr, "[sgx]     +0x%03x: не відображено\n", off);
            continue;
        }
        for (k = 0; k < n; k++) {
            w[k] = sgx_phys_ld32(pa + 4 * k);
        }
        fprintf(stderr, "[sgx]     +0x%03x (PA %08x):", off, pa);
        for (k = 0; k < n; k++) {
            fprintf(stderr, " %08x", w[k]);
        }
        fprintf(stderr, "\n");
    }
}

/* --- прилади пошуку (діагностика, у поведінці моделі не бере участі) --- */

/*
 * Кодування, у яких SGX узагалі буває записана адреса. Два з них доведені
 * прогоном: `EVENT_KICKER` тримає device-VA як є, а регістр 0x0A68 — байтовий
 * зсув від `PDS_EXEC_BASE`. Решта — форми, які треба перевірити, а не
 * вважати істиною; саме тому це прилад, а не механізм.
 */
typedef struct ClarionSgxEnc {
    const char *name;
    uint32_t value;
    bool valid;
} ClarionSgxEnc;

static unsigned sgx_encodings(uint32_t target, uint32_t pds, uint32_t use,
                              ClarionSgxEnc *out, unsigned max)
{
    unsigned n = 0;

    if (n < max) { out[n++] = (ClarionSgxEnc){"абсолютний", target, true}; }
    if (pds && target >= pds) {
        uint32_t d = target - pds;
        if (n < max) { out[n++] = (ClarionSgxEnc){"PDS-rel", d, true}; }
        if (n < max) { out[n++] = (ClarionSgxEnc){"PDS-rel>>2", d >> 2, true}; }
        if (n < max) { out[n++] = (ClarionSgxEnc){"PDS-rel>>4", d >> 4, true}; }
    }
    if (use && target >= use) {
        uint32_t d = target - use;
        if (n < max) { out[n++] = (ClarionSgxEnc){"USE-rel", d, true}; }
        if (n < max) { out[n++] = (ClarionSgxEnc){"USE-rel>>2", d >> 2, true}; }
        if (n < max) { out[n++] = (ClarionSgxEnc){"USE-rel>>4", d >> 4, true}; }
    }
    return n;
}

/*
 * QY8_SGX_FIND=VA — хто взагалі посилається на цей device-VA. Дивимось і в
 * зафіксовані записи регістрів, і в кожне вирівняне слово всіх відображених
 * сторінок. Нічого не «знаходимо» для самої моделі: це відповідь інженерові.
 */
static void sgx_find_target(ClarionSgxState *s, uint32_t pd, uint32_t pds,
                            uint32_t use, uint32_t target)
{
    ClarionSgxEnc enc[8];
    unsigned nenc = sgx_encodings(target, pds, use, enc, ARRAY_SIZE(enc));
    uint32_t field = use && target >= use ? ((target - use) >> 4) & 0xFFF : 0;
    unsigned i, hits = 0;

    fprintf(stderr, "[sgx]   FIND %08x — кодування:", target);
    for (i = 0; i < nenc; i++) {
        fprintf(stderr, " %s=%08x", enc[i].name, enc[i].value);
    }
    fprintf(stderr, "\n");

    for (i = 0; i < SGX_NREGS; i++) {
        unsigned k;

        if (!s->regs[i]) {
            continue;
        }
        for (k = 0; k < nenc; k++) {
            if (enc[k].value && s->regs[i] == enc[k].value) {
                fprintf(stderr, "[sgx]     регістр +0x%04x = %08x  (%s)\n",
                        i * 4, s->regs[i], enc[k].name);
                hits++;
            }
        }
    }

    for (i = 0; i < SGX_MMU_ENTRIES; i++) {
        uint32_t pde = sgx_phys_ld32(pd + 4 * i);
        uint32_t pt_pa;
        unsigned j;

        if (!(pde & SGX_MMU_ENTRY_VALID)) {
            continue;
        }
        pt_pa = pde & SGX_MMU_ENTRY_ADDR_MASK;
        for (j = 0; j < SGX_MMU_ENTRIES; j++) {
            uint32_t pte = sgx_phys_ld32(pt_pa + 4 * j);
            uint32_t page_va = (i << SGX_MMU_PD_SHIFT) | (j << SGX_MMU_PAGE_SHIFT);
            uint32_t pa;
            unsigned o;

            if (!(pte & SGX_MMU_ENTRY_VALID)) {
                continue;
            }
            pa = pte & SGX_MMU_ENTRY_ADDR_MASK;
            for (o = 0; o < SGX_MMU_PAGE_SIZE; o += 4) {
                uint32_t w = sgx_phys_ld32(pa + o);
                unsigned k;

                if (!w) {
                    continue;
                }
                for (k = 0; k < nenc; k++) {
                    if (enc[k].value && w == enc[k].value) {
                        fprintf(stderr, "[sgx]     пам'ять GPU VA %08x = %08x"
                                "  (%s)\n", page_va + o, w, enc[k].name);
                        hits++;
                    }
                }
                /*
                 * Слабший, 12-бітний варіант: поле [15:4] у парі вигляду
                 * 0x0020XXYF/0x04000000. Показуємо окремо і підписуємо як
                 * слабкий — 12 бітів самі по собі нічого не доводять.
                 */
                if (field && (w & 0xFFFF000F) == 0x0020000F &&
                    ((w >> 4) & 0xFFF) == field) {
                    fprintf(stderr, "[sgx]     пам'ять GPU VA %08x = %08x"
                            "  (поле[15:4]=%03x, СЛАБКИЙ збіг)\n",
                            page_va + o, w, field);
                    hits++;
                }
            }
        }
    }
    fprintf(stderr, "[sgx]     збігів: %u\n", hits);
}

/*
 * QY8_SGX_GRAPH=1 — кожне вирівняне слово відображеної пам'яті, яке саме є
 * валідним device-VA в поточному каталозі, тобто ребро графа вказівників.
 * Так знаходять посилання, про які ще не здогадались питати.
 */
static void sgx_report_graph(uint32_t pd)
{
    unsigned i, edges = 0;

    fprintf(stderr, "[sgx]   граф вказівників (слово = валідний device-VA):\n");
    for (i = 0; i < SGX_MMU_ENTRIES; i++) {
        uint32_t pde = sgx_phys_ld32(pd + 4 * i);
        uint32_t pt_pa;
        unsigned j;

        if (!(pde & SGX_MMU_ENTRY_VALID)) {
            continue;
        }
        pt_pa = pde & SGX_MMU_ENTRY_ADDR_MASK;
        for (j = 0; j < SGX_MMU_ENTRIES; j++) {
            uint32_t pte = sgx_phys_ld32(pt_pa + 4 * j);
            uint32_t page_va = (i << SGX_MMU_PD_SHIFT) | (j << SGX_MMU_PAGE_SHIFT);
            uint32_t pa, tgt;
            unsigned o;

            if (!(pte & SGX_MMU_ENTRY_VALID)) {
                continue;
            }
            pa = pte & SGX_MMU_ENTRY_ADDR_MASK;
            for (o = 0; o < SGX_MMU_PAGE_SIZE; o += 4) {
                uint32_t w = sgx_phys_ld32(pa + o);

                /* Вирівняність на 4 відсіює переважну більшість коду. */
                if (!w || (w & 3) || !sgx_translate(pd, w, &tgt)) {
                    continue;
                }
                fprintf(stderr, "[sgx]     %08x -> %08x (PA %08x)\n",
                        page_va + o, w, tgt);
                edges++;
            }
        }
    }
    fprintf(stderr, "[sgx]     ребер: %u\n", edges);
}

/* --- розбір програми PDS (усе за публічним DDK, гілка SGX540) --------- */

/*
 * Зсув константи ds0[k] у сегменті даних, у двійних словах. Дослівно
 * `PDSGetDS0ConstantOffset()` з `eurasia/codegen/pds/pds.c`:
 *
 *     row    = k / PDS_NUM_DWORDS_PER_ROW;
 *     column = k % PDS_NUM_DWORDS_PER_ROW;
 *     return (2 * row) * PDS_NUM_DWORDS_PER_ROW + column;
 *
 * Для SGX540 `PDS_NUM_DWORDS_PER_ROW = 2`, тобто банки ds0/ds1 чергуються
 * парами двійних слів. Саме тому ds0[2] лежить у слові 4, а не 2.
 */
static uint32_t pds_ds0_dword(uint32_t k)
{
    uint32_t row = k / PDS_NUM_DWORDS_PER_ROW;
    uint32_t col = k % PDS_NUM_DWORDS_PER_ROW;

    return (2 * row) * PDS_NUM_DWORDS_PER_ROW + col;
}

typedef struct ClarionPdsCtx {
    uint32_t pd;                /* каталог сторінок SGX */
    uint32_t data_va;           /* база сегмента даних = база об'єкта */
    uint32_t ndwords;           /* розмір сегмента даних у двійних словах */
    uint32_t use_base[16];      /* декодовані EUR_CR_USE_CODE_BASE_0..15 */
    uint32_t temp[64];          /* приймачі MOV32, які ми відстежили */
    bool temp_known[64];
} ClarionPdsCtx;

/* Значення константи ds0[k], якщо вона в межах сегмента даних. */
static bool pds_ds0(ClarionPdsCtx *c, uint32_t k, uint32_t *out)
{
    uint32_t dw = pds_ds0_dword(k);
    uint32_t pa;

    if (dw >= c->ndwords || !sgx_translate(c->pd, c->data_va + 4 * dw, &pa)) {
        return false;
    }
    *out = sgx_phys_ld32(pa);
    return true;
}

/* `EUR_CR_USE_CODE_BASE(x)`: поле ADDR — біти 23:0, device-VA = ADDR << 8. */
static uint32_t pds_use_code_base(ClarionSgxState *s, unsigned i)
{
    uint32_t v = sgx_reg(s, SGX_CR_USE_CODE_BASE(i));

    return (v & SGX_USE_CODE_BASE_ADDR_MASK) << 8;
}

static void pds_report_doutd(ClarionPdsCtx *c, const uint32_t *emit, unsigned n)
{
    uint32_t sbase, ctl, bsize, blines, ao, stride, bytes, pa;

    if (n < PDS_NUM_DMA_CONTROL_WORDS) {
        fprintf(stderr, "[sgx]       DOUTD: операнди не розв'язані\n");
        return;
    }
    sbase = emit[0];
    ctl = emit[1];
    bsize = (ctl & PDS_DOUTD1_BSIZE_MASK) + 1;
    blines = ((ctl >> PDS_DOUTD1_BLINES_SHIFT) & PDS_DOUTD1_BLINES_MASK) + 1;
    ao = (ctl >> PDS_DOUTD1_AO_SHIFT) & PDS_DOUTD1_AO_MASK;
    stride = (ctl >> PDS_DOUTD1_STRIDE_SHIFT) & PDS_DOUTD1_STRIDE_MASK;
    bytes = bsize * blines * 4;

    fprintf(stderr, "[sgx]       DOUTD: SBASE=%08x  DOUTD1=%08x\n", sbase, ctl);
    fprintf(stderr, "[sgx]              BSIZE=%u BLINES=%u AO=%u STRIDE=%u INSTR=%u"
            " -> %u Б з %08x\n", bsize, blines, ao, stride,
            (ctl >> PDS_DOUTD1_INSTR_SHIFT) & 3, bytes, sbase);
    if (sgx_translate(c->pd, sbase, &pa)) {
        fprintf(stderr, "[sgx]              джерело DMA відображене: PA %08x\n", pa);
    } else {
        fprintf(stderr, "[sgx]              ⚠ джерело DMA НЕ відображене в цьому"
                " каталозі\n");
    }
}

static void pds_report_doutu(ClarionPdsCtx *c, const uint32_t *emit, unsigned n)
{
    uint32_t w0, w1, cbase, coff, exe, exeaddr, va;

    if (n < PDS_NUM_USE_TASK_CONTROL_WORDS) {
        fprintf(stderr, "[sgx]       DOUTU: операнди не розв'язані\n");
        return;
    }
    w0 = emit[0];
    w1 = emit[1];
    cbase = w0 & PDS_DOUTU0_CBASE_MASK;
    coff = (w0 >> PDS_DOUTU0_COFF_SHIFT) & PDS_DOUTU0_COFF_MASK;
    exe = (w0 >> PDS_DOUTU0_EXE_SHIFT) & PDS_DOUTU0_EXE_MASK;
    exeaddr = (coff << PDS_DOUTU0_COFF_ALIGNSHIFT) |
              (exe << PDS_DOUTU0_EXE_ALIGNSHIFT);
    va = c->use_base[cbase] + exeaddr;

    fprintf(stderr, "[sgx]       DOUTU: %08x %08x %08x\n", w0, w1, emit[2]);
    fprintf(stderr, "[sgx]              CBASE=%u -> USE_CODE_BASE_%u=%08x;"
            " COFF=%x EXE=%03x -> зсув 0x%05x\n",
            cbase, cbase, c->use_base[cbase], coff, exe, exeaddr);
    fprintf(stderr, "[sgx]              ➜ ТОЧКА ВХОДУ ЗАДАЧІ USE: device VA %08x%s\n",
            va, (w0 & PDS_DOUTU0_PDSDMADEP) ? "  [PDSDMADEPENDENCY]" : "");
    fprintf(stderr, "[sgx]              MODE=%s\n",
            (w1 >> PDS_DOUTU1_MODE_SHIFT) & 1 ? "PERINSTANCE" : "PARALLEL");
}

/*
 * Розібрати програму PDS, на яку показує task-control із MMIO. Це РОЗБІР, не
 * виконання: ми не змінюємо жодного байта пам'яті гостя, не робимо DMA і не
 * запускаємо задачі USE. Мета — назвати точку входу задачі USE і описати DMA,
 * бо саме DOUTD переносить у задачу вміст об'єкта, на який показує сегмент
 * даних.
 */
static void sgx_pds_report(ClarionSgxState *s, uint32_t pd, uint32_t pds_base)
{
    ClarionPdsCtx c;
    uint32_t rows = sgx_reg(s, SGX_CR_QY8_TASK_W1);
    uint32_t off, code_va, i;
    unsigned n;

    memset(&c, 0, sizeof(c));
    c.pd = pd;
    c.data_va = pds_base + sgx_reg(s, SGX_CR_QY8_TASK_ADDR);
    /* Рядок займає PDS_NUM_DWORDS_PER_ROW двійних слів у КОЖНОМУ з двох банків. */
    c.ndwords = rows * 2 * PDS_NUM_DWORDS_PER_ROW;
    for (i = 0; i < 16; i++) {
        c.use_base[i] = pds_use_code_base(s, i);
    }
    code_va = c.data_va + 4 * c.ndwords;

    fprintf(stderr, "[sgx]   розбір програми PDS:\n");
    fprintf(stderr, "[sgx]     сегмент даних %08x, рядків %u -> %u двійних слів"
            " (%u Б)\n", c.data_va, rows, c.ndwords, 4 * c.ndwords);
    fprintf(stderr, "[sgx]     код з %08x\n", code_va);

    for (off = 0; off < 0x100; off += PDS_INSTRUCTION_SIZE) {
        uint32_t pa, w, group, type, cc;

        if (!sgx_translate(pd, code_va + off, &pa)) {
            fprintf(stderr, "[sgx]     +0x%02x: не відображено — розбір спинено\n",
                    off);
            return;
        }
        w = sgx_phys_ld32(pa);
        group = (w >> PDS_INST_SHIFT) & 3;
        type = (w >> PDS_TYPE_SHIFT) & 7;
        cc = (w >> PDS_CC_SHIFT) & 7;

        fprintf(stderr, "[sgx]     +0x%02x: %08x  група=%u тип=%u cc=%u", off, w,
                group, type, cc);

        if (group == PDS_INST_FLOW && type == PDS_TYPE_HALT) {
            fprintf(stderr, "  HALT\n");
            return;
        }
        if (group == PDS_INST_FLOW && type == PDS_TYPE_TSTZ) {
            fprintf(stderr, "  TSTZ\n");
            continue;
        }
        if (group == PDS_INST_FLOW && type == PDS_TYPE_BRA) {
            fprintf(stderr, "  BRA -> %u\n", w & 0xFFFFFF);
            continue;
        }
        if (group == PDS_INST_MOV && type == PDS_TYPE_MOV32) {
            uint32_t src = (w >> PDS_MOV32_SRC_SHIFT) & PDS_MOV32_SRC_MASK;
            uint32_t dst = (w >> PDS_MOV32_DEST_SHIFT) & PDS_MOV32_DEST_MASK;
            uint32_t val;

            fprintf(stderr, "  MOV32 dest[%u] <- src[%u]", dst, src);
            if (!((w >> PDS_MOV32_SRCSEL_SHIFT) & 3) && pds_ds0(&c, src, &val)) {
                c.temp[dst] = val;
                c.temp_known[dst] = true;
                fprintf(stderr, " = %08x", val);
            }
            fprintf(stderr, "\n");
            continue;
        }
        if (group == PDS_INST_MOV && type == PDS_TYPE_MOVS) {
            uint32_t src1 = (w >> PDS_MOVS_SRC1_SHIFT) & PDS_MOVS_SRC1_MASK;
            uint32_t src2 = (w >> PDS_MOVS_SRC2_SHIFT) & PDS_MOVS_SRC2_MASK;
            bool s1_ds0 = !((w >> PDS_MOVS_SRC1SEL_SHIFT) & 1);
            uint32_t dest = w & PDS_MOVS_DEST_MASK;
            /* Індекси двійних слів: джерела адресують ЧЕТВЕРНІ слова. */
            uint32_t d1 = src1 * PDS_NUM_DWORDS_PER_QWORD;
            uint32_t d2 = src2 * PDS_NUM_DWORDS_PER_QWORD;
            uint32_t pair[4] = { 0, 0, 0, 0 };
            bool have[4] = { false, false, false, false };
            uint32_t emit[4] = { 0, 0, 0, 0 };
            unsigned k;

            /* SRC1 — пара констант банку DS0 сегмента даних. */
            if (s1_ds0) {
                have[PDS_MOVS_SWIZ_SRC1L] = pds_ds0(&c, d1, &pair[0]);
                have[PDS_MOVS_SWIZ_SRC1H] = pds_ds0(&c, d1 + 1, &pair[1]);
            }
            /*
             * SRC2 — завжди банк DS1. Константи DS1 сегмента даних ми не
             * розв'язуємо (розкладки для DS1 у цій копії DDK немає), а от
             * тимчасові (індекс >= TEMPSTART) ми відстежили через MOV32 —
             * саме через них PDS і передає третє слово task-control.
             */
            if (d2 >= PDS_DATASTORE_TEMPSTART && d2 + 1 < ARRAY_SIZE(c.temp)) {
                have[PDS_MOVS_SWIZ_SRC2L] = c.temp_known[d2];
                pair[2] = c.temp[d2];
                have[PDS_MOVS_SWIZ_SRC2H] = c.temp_known[d2 + 1];
                pair[3] = c.temp[d2 + 1];
            }

            n = 0;
            for (k = 0; k < 4; k++) {
                uint32_t sw = (w >> PDS_MOVS_SWIZ_SHIFT(k)) & 3;

                if (!have[sw]) {
                    break;
                }
                emit[k] = pair[sw];
                n++;
            }

            fprintf(stderr, "  MOVS dest=%u src1=%s[%u] src2=ds1[%u]"
                    " (розв'язано %u)\n",
                    dest, s1_ds0 ? "ds0" : "reg", d1, d2, n);
            if (dest == PDS_MOVS_DEST_DOUTD) {
                pds_report_doutd(&c, emit, n);
            } else if (dest == PDS_MOVS_DEST_DOUTU) {
                pds_report_doutu(&c, emit, n);
            }
            continue;
        }
        fprintf(stderr, "  (не тлумачимо)\n");
    }
    fprintf(stderr, "[sgx]     HALT не знайдено в межах 0x100 Б — розбір спинено\n");
}

/* --- звіт на kick ----------------------------------------------------- */

static void sgx_report_kick(ClarionSgxState *s)
{
    uint32_t pds   = sgx_reg(s, SGX_CR_PDS_EXEC_BASE) &
                     SGX_PDS_EXEC_BASE_ADDR_MASK;
    uint32_t task  = sgx_reg(s, SGX_CR_QY8_TASK_ADDR);
    uint32_t use15 = sgx_reg(s, SGX_CR_USE_CODE_BASE(15));
    uint32_t kicker = sgx_reg(s, SGX_CR_EVENT_KICKER) &
                      SGX_EVENT_KICKER_ADDR_MASK;
    uint32_t pd    = sgx_reg(s, SGX_CR_BIF_DIR_LIST_BASE0) &
                     SGX_BIF_DIR_LIST_BASE_ADDR_MASK;
    uint32_t root  = pds + task;
    uint32_t pa;
    unsigned i;

    fprintf(stderr, "[sgx] EVENT_KICK2 #%u — розбір видимого стану\n",
            s->kicks);
    fprintf(stderr, "[sgx]   EVENT_HOST_ENABLE  = %08x   (біт 14 = SW event)\n",
            sgx_reg(s, SGX_CR_EVENT_HOST_ENABLE));
    fprintf(stderr, "[sgx]   EVENT_KICKER       = %08x   device-VA лічильника\n",
            kicker);
    fprintf(stderr, "[sgx]   PDS_EXEC_BASE      = %08x\n", pds);
    fprintf(stderr, "[sgx]   USE_CODE_BASE_15   = %08x   DM=%u ADDR<<8=%08x\n",
            use15,
            (use15 & SGX_USE_CODE_BASE_DM_MASK) >> SGX_USE_CODE_BASE_DM_SHIFT,
            (use15 & SGX_USE_CODE_BASE_ADDR_MASK) << 8);
    fprintf(stderr, "[sgx]   рег 0x0A68/6C/70/74 = %08x %08x %08x %08x\n",
            task, sgx_reg(s, SGX_CR_QY8_TASK_W1),
            sgx_reg(s, SGX_CR_QY8_TASK_W2), sgx_reg(s, SGX_CR_QY8_TASK_W3));
    fprintf(stderr, "[sgx]   корінь = PDS_EXEC_BASE + рег 0x0A68 = %08x\n",
            root);
    fprintf(stderr, "[sgx]   BIF_DIR_LIST_BASE0 = %08x\n", pd);

    if (s->show_writes) {
        /*
         * Повний журнал у порядку надходження. Потрібен тому, що init-script
         * — не єдине джерело записів: `SGXReset` пише частину регістрів
         * (зокрема BIF_DIR_LIST_BASE0) прямо, і в таблиці скрипта їх немає.
         */
        fprintf(stderr, "[sgx]   журнал записів (%u%s):\n", s->nwr,
                s->wr_overflow ? ", ПЕРЕПОВНЕНО" : "");
        for (i = 0; i < s->nwr; i++) {
            fprintf(stderr, "[sgx]     %3u  +0x%04x <- %08x\n",
                    i, s->wr[i].off, s->wr[i].val);
        }
    }

    if (!pd) {
        /*
         * Гість не писав каталог у це вікно. Нічого не вигадуємо: без
         * каталогу жоден device-VA перекласти неможливо, і так і кажемо.
         */
        fprintf(stderr, "[sgx]   каталог сторінок не записаний у цей блок — "
                "обхід MMU неможливий\n");
        return;
    }

    sgx_report_pd(pd);

    if (!sgx_translate(pd, root, &pa)) {
        fprintf(stderr, "[sgx]   корінь %08x НЕ відображений у цьому "
                "каталозі\n", root);
    } else {
        sgx_report_range(pd, "корінь", root, 0x40);
    }

    for (i = 0; i < s->ndump; i++) {
        sgx_report_range(pd, "QY8_SGX_DUMP", s->dump[i].va, s->dump[i].len);
    }
    for (i = 0; i < s->nfind; i++) {
        sgx_find_target(s, pd, pds, (use15 & SGX_USE_CODE_BASE_ADDR_MASK) << 8,
                        s->find[i]);
    }
    if (s->graph) {
        sgx_report_graph(pd);
    }
    if (pds && sgx_reg(s, SGX_CR_QY8_TASK_ADDR)) {
        sgx_pds_report(s, pd, pds);
    }
}

/* --- MMIO ------------------------------------------------------------- */

static uint64_t sgx_read(void *opaque, hwaddr addr, unsigned size)
{
    ClarionSgxState *s = opaque;
    uint64_t val = 0;

    if (s->readback && size == 4 && addr + 4 <= CLARION_SGX_SIZE) {
        val = sgx_reg(s, addr);
    }

    /*
     * Читання лишається журнальованим під LOG_UNIMP, і це не формальність:
     * ми справді нічого не моделюємо на читання, а `-d unimp` — той самий
     * прапорець, яким знято всі попередні траси цього блока. Рядок тримаємо
     * у формі широкого перехоплювача, щоб старі A/B-дифи далі порівнювались.
     */
    qemu_log_mask(LOG_UNIMP, "clarion-sgx: read  (size %d, offset 0x%04"
                  HWADDR_PRIx ") -> 0x%0*" PRIx64 "\n",
                  size, addr, size << 1, val);
    trace_clarion_sgx_read((uint32_t)addr, size, val);
    return val;
}

static void sgx_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    ClarionSgxState *s = opaque;

    qemu_log_mask(LOG_UNIMP, "clarion-sgx: write (size %d, offset 0x%04"
                  HWADDR_PRIx ", value 0x%0*" PRIx64 ")\n",
                  size, addr, size << 1, val);
    trace_clarion_sgx_write((uint32_t)addr, size, val);

    if (size == 4 && addr + 4 <= CLARION_SGX_SIZE) {
        s->regs[addr / 4] = (uint32_t)val;
        if (s->nwr < SGX_WR_JOURNAL) {
            s->wr[s->nwr].off = (uint32_t)addr;
            s->wr[s->nwr].val = (uint32_t)val;
            s->nwr++;
        } else {
            s->wr_overflow = true;
        }
    }

    if (addr == SGX_CR_EVENT_KICK2 && (val & SGX_CR_EVENT_KICK2_NOW)) {
        s->kicks++;
        if (s->kicks <= s->kick_reports) {
            sgx_report_kick(s);
        } else {
            fprintf(stderr, "[sgx] EVENT_KICK2 #%u (розбір пропущено, "
                    "QY8_SGX_KICKS=%u)\n", s->kicks, s->kick_reports);
        }
    }
}

static const MemoryRegionOps sgx_ops = {
    .read = sgx_read,
    .write = sgx_write,
    /*
     * Ті самі межі, що й у широкого перехоплювача плати: прошивка ходить до
     * цього блока не лише 32-бітними доступами (у трасах є 8-байтові читання
     * зсувів 0x100..0x1F8), і модель не має права звузити те, що гість уже
     * робить.
     */
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/* --- QOM -------------------------------------------------------------- */

static void clarion_sgx_reset_hold(Object *obj, ResetType type)
{
    ClarionSgxState *s = CLARION_SGX(obj);

    memset(s->regs, 0, sizeof(s->regs));
    s->kicks = 0;
    s->nwr = 0;
    s->wr_overflow = false;
}

/* QY8_SGX_DUMP="0x0F003000:0x104,0x0E40C1B0:0x4C" */
static void sgx_parse_dump(ClarionSgxState *s, const char *spec)
{
    while (spec && *spec && s->ndump < SGX_DUMP_RANGES) {
        char *end;
        uint64_t va = strtoull(spec, &end, 0);
        uint64_t len = 0x40;

        if (end == spec) {
            break;
        }
        if (*end == ':') {
            spec = end + 1;
            len = strtoull(spec, &end, 0);
        }
        s->dump[s->ndump].va = (uint32_t)va;
        s->dump[s->ndump].len = (uint32_t)len;
        s->ndump++;
        if (*end != ',') {
            break;
        }
        spec = end + 1;
    }
}

/* QY8_SGX_FIND="0x0E40C1B0,0x0F003000" */
static void sgx_parse_find(ClarionSgxState *s, const char *spec)
{
    while (spec && *spec && s->nfind < SGX_FIND_TARGETS) {
        char *end;
        uint64_t va = strtoull(spec, &end, 0);

        if (end == spec) {
            break;
        }
        s->find[s->nfind++] = (uint32_t)va;
        if (*end != ',') {
            break;
        }
        spec = end + 1;
    }
}

static void clarion_sgx_realize(DeviceState *dev, Error **errp)
{
    ClarionSgxState *s = CLARION_SGX(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    const char *e;

    e = getenv("QY8_SGX_KICKS");
    s->kick_reports = e ? (uint32_t)atoi(e) : 2;
    s->readback = getenv("QY8_SGX_READBACK") != NULL;
    s->show_writes = getenv("QY8_SGX_WRITES") != NULL;
    s->graph = getenv("QY8_SGX_GRAPH") != NULL;
    sgx_parse_dump(s, getenv("QY8_SGX_DUMP"));
    sgx_parse_find(s, getenv("QY8_SGX_FIND"));

    if (s->readback) {
        fprintf(stderr, "[sgx] ⚠ QY8_SGX_READBACK: читання віддають записане — "
                "видима гостем поведінка ЗМІНЕНА\n");
    }

    memory_region_init_io(&s->mr, OBJECT(dev), &sgx_ops, s,
                          "clarion-sgx", CLARION_SGX_SIZE);
    sysbus_init_mmio(sbd, &s->mr);
}

static void clarion_sgx_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = clarion_sgx_realize;
    dc->desc = "Clarion QY8XXX PowerVR SGX (passive MMIO + BIF/MMU tracer)";
    rc->phases.hold = clarion_sgx_reset_hold;
}

static const TypeInfo clarion_sgx_types[] = {
    {
        .name          = TYPE_CLARION_SGX,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(ClarionSgxState),
        .class_init    = clarion_sgx_class_init,
    },
};

DEFINE_TYPES(clarion_sgx_types)
