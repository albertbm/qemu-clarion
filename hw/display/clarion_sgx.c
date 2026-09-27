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
 * за проходження перевірки: зашита адреса або сканування пам'яті за підписом
 * заборонені контрактом проєкту. Діра в ланцюгу, через яку M3 довго був
 * нереалізовний, тепер ЗАКРИТА, і ось чим саме:
 *
 *   EUR_CR_PDS_EXEC_BASE      (0x0AB8) = 0x0DC00000       <- MMIO
 *   EUR_CR_EVENT_OTHER_PDS_EXEC(0x0A68) = 0x0080C180      <- MMIO
 *   сума                               = 0x0E40C180        = програма PDS #13
 *   #13 = sgxinit_primary: MOVS DOUTU  -> задача USE @0x0E400BA0
 *   код USE: emitpds                   -> програма PDS #14 @0x0E40C1B0
 *   #14 = sgxinit_secondary: MOVS DOUTD -> 128 Б з 0x0F003000 у вторинні
 *                                          атрибути (SGX_UKERNEL_NUM_SEC_ATTRIB)
 *   вторинний атрибут sa[1] = R_HostCtl = 0x0F003120       = HOST_CTL
 *   HOST_CTL +0x00                      = ui32InitStatus
 *
 * Тобто адресу GPU дістає ВИКОНАННЯМ задокументованого DMA, а не тим, що її
 * хтось вписав у модель. Розкладку вторинних атрибутів задає
 * `PVRSRV_SGX_EDMPROG_SECATTR` (`sgx_mkif.h:144`), ім'я `R_HostCtl = SA(sHostCtl)`
 * — `usedefs.h:70`. Повний розбір і критерії — docs/sgx/09, 22 і 24
 * репозиторію nissan-can-explore.
 *
 * Що вже реалізовано (M3-B1) і чого ще немає (M3-B2). Під QY8_SGX_EXEC модель
 * ВИКОНУЄ програми PDS: MOV32, TSTZ/TSTN, BRA/CALL/RTN, HALT, DOUTD (справжнє
 * копіювання у банк вторинних атрибутів) і DOUTU (постановка точки входу
 * задачі USE в чергу). Інтерпретатора USE ще немає, тому ланка `emitpds`
 * поки розімкнена, і `ui32InitStatus` модель не чіпає: навіть із
 * QY8_SGX_EXEC=1 гість бачить рівно те саме, що й без нього.
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
 *   QY8_SGX_EXEC=1           виконувати програми PDS, а не лише розбирати.
 *                            Наслідки лишаються в стані МОДЕЛІ (банк вторинних
 *                            атрибутів, черга задач USE); у пам'ять гостя
 *                            модель не пише, тож бут A/B не змінюється.
 *   QY8_SGX_PDS_RUN=VA[:рядків[:ir0]]
 *                            ⚠ діагностика, не ланка чесного ланцюга: виконати
 *                            названу програму PDS. Потрібне, доки немає
 *                            інтерпретатора USE і `emitpds` не може сам
 *                            запустити sgxinit_secondary.
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
#define SGX_USE_QUEUE       16

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

    /* --- M3-B1: виконання PDS (QY8_SGX_EXEC) ------------------------- */

    bool exec;                  /* виконувати програми PDS, а не лише розбирати */

    /*
     * Банк вторинних атрибутів мікроядра. Це стан МОДЕЛІ, не пам'ять гостя:
     * DOUTD наповнює його, а код USE (M3-B2) читатиме з нього базу для
     * `stad [sa1,+#0]`. Гість цього банку не бачить, тому M3-B1 нічого в
     * його поведінці не змінює.
     */
    uint32_t sa[SGX_SA_DWORDS];
    bool sa_known[SGX_SA_DWORDS];
    unsigned sa_count;
    uint32_t sa_sbase;          /* SBASE останнього DOUTD — для самоперевірки */

    /* Точки входу задач USE, які запустив DOUTU. Виконання — M3-B2. */
    uint32_t use_queue[SGX_USE_QUEUE];
    unsigned nuse;

    /* QY8_SGX_PDS_RUN — діагностичний запуск названої програми PDS. */
    uint32_t run_va;
    uint32_t run_rows;
    uint32_t run_ir0;
    bool run_set;
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

/* --- програма PDS: розбір і виконання (усе за публічним DDK, SGX540) --- */

/*
 * Стан виконання програми PDS.
 *
 * Datastore — два банки по PDS_DATASTORE_PERBANKSIZE двійних слів. Індекси
 * 0..47 — константи, що фізично лежать у сегменті даних програми; 48..63 —
 * тимчасові, яких у пам'яті немає взагалі. Обидві розкладки — дослівно
 * `PDSGetDS0ConstantOffset`/`PDSGetDS1ConstantOffset` з `codegen/pds/pds.c`.
 */
typedef struct ClarionPdsCtx {
    ClarionSgxState *s;
    uint32_t pd;                /* каталог сторінок SGX */
    uint32_t data_va;           /* база сегмента даних = база об'єкта */
    uint32_t ndwords;           /* розмір сегмента даних у двійних словах */
    uint32_t code_va;           /* база сегмента коду */
    uint32_t use_base[16];      /* декодовані EUR_CR_USE_CODE_BASE_0..15 */

    bool exec;                  /* виконувати, а не лише розбирати */

    uint32_t temp[2][PDS_DATASTORE_PERBANKSIZE];
    bool temp_known[2][PDS_DATASTORE_PERBANKSIZE];

    bool pred[3];               /* p0..p2 */
    bool pred_known[3];
    uint32_t ir[2];             /* вхідні регістри задачі */
    bool ir_known[2];

    const char *stop;           /* чому виконання спинено, або NULL */
} ClarionPdsCtx;

/*
 * Зсув константи ds<bank>[k] у сегменті даних, у двійних словах. Дослівно
 * `PDSGetDS0ConstantOffset()`/`PDSGetDS1ConstantOffset()` з
 * `eurasia/codegen/pds/pds.c`:
 *
 *     row    = k / PDS_NUM_DWORDS_PER_ROW;
 *     column = k % PDS_NUM_DWORDS_PER_ROW;
 *     ds0: (2 * row)     * PDS_NUM_DWORDS_PER_ROW + column;
 *     ds1: (2 * row + 1) * PDS_NUM_DWORDS_PER_ROW + column;
 *
 * Для SGX540 `PDS_NUM_DWORDS_PER_ROW = 2`, тобто банки ds0/ds1 чергуються
 * парами двійних слів. Саме тому ds0[2] лежить у слові 4, а не 2.
 */
static uint32_t pds_ds_dword(unsigned bank, uint32_t k)
{
    uint32_t row = k / PDS_NUM_DWORDS_PER_ROW;
    uint32_t col = k % PDS_NUM_DWORDS_PER_ROW;

    return (2 * row + (bank ? 1 : 0)) * PDS_NUM_DWORDS_PER_ROW + col;
}

/*
 * Прочитати ds<bank>[k]. Константа приходить із сегмента даних, тимчасова —
 * з нашого стану. Якщо значення невідоме, повертаємо false і НІЧОГО не
 * вигадуємо: далі виконання спиниться і скаже, на чому саме.
 */
static bool pds_ds_read(ClarionPdsCtx *c, unsigned bank, uint32_t k,
                        uint32_t *out)
{
    uint32_t dw, pa;

    if (bank > 1 || k >= PDS_DATASTORE_PERBANKSIZE) {
        return false;
    }
    if (k >= PDS_DATASTORE_TEMPSTART) {
        if (!c->temp_known[bank][k]) {
            return false;
        }
        *out = c->temp[bank][k];
        return true;
    }
    dw = pds_ds_dword(bank, k);
    if (dw >= c->ndwords || !sgx_translate(c->pd, c->data_va + 4 * dw, &pa)) {
        return false;
    }
    *out = sgx_phys_ld32(pa);
    return true;
}

/*
 * Записати ds<bank>[k]. Писати можна лише в тимчасові: константи лежать у
 * пам'яті гостя, і модель у неї не пише (M3-B1 нічого гостю не змінює).
 */
static bool pds_ds_write(ClarionPdsCtx *c, unsigned bank, uint32_t k,
                         uint32_t val)
{
    if (bank > 1 || k < PDS_DATASTORE_TEMPSTART ||
        k >= PDS_DATASTORE_PERBANKSIZE) {
        return false;
    }
    c->temp[bank][k] = val;
    c->temp_known[bank][k] = true;
    return true;
}

/*
 * Чи виконується інструкція за своїм полем cc (`sgxdefs.h:2653..2662`).
 * `known` віддає false, якщо умова спирається на те, чого ми не знаємо —
 * тоді виконання спиняється, а не вгадує гілку.
 */
static bool pds_cc_true(ClarionPdsCtx *c, uint32_t cc, bool *known)
{
    *known = true;
    switch (cc) {
    case PDS_CC_ALWAYS:
        return true;
    case PDS_CC_P0:
    case PDS_CC_P1:
    case PDS_CC_P2:
        *known = c->pred_known[cc];
        return c->pred[cc];
    default:
        /*
         * IF0/IF1 (стан зовнішніх інтерфейсів) і ALUZ/ALUN (прапорці
         * арифметики) у цій черзі задач не трапляються. Не вгадуємо.
         */
        *known = false;
        return false;
    }
}

/* `EUR_CR_USE_CODE_BASE(x)`: поле ADDR — біти 23:0, device-VA = ADDR << 8. */
static uint32_t pds_use_code_base(ClarionSgxState *s, unsigned i)
{
    uint32_t v = sgx_reg(s, SGX_CR_USE_CODE_BASE(i));

    return (v & SGX_USE_CODE_BASE_ADDR_MASK) << 8;
}

/*
 * DOUTD — DMA у банк вторинних атрибутів.
 *
 * Поля — `sgxdefs.h:4211..4275`, гілка НЕ-SGX545/543: BSIZE біти 3:0, BLINES
 * 7:4, AO 18:8, INSTR 20:19, STRIDE 29:21, STYPE біт 30. Усі три розміри
 * закодовані як «мінус один» — це видно з самого будівника нашої програми
 * `srvinit/devices/sgx/sgxinit.c:1961..1973`, де в поля кладуть
 * `ui32DMABurstSize - 1`, `ui32DMABurstLines - 1` і знову
 * `ui32DMABurstSize - 1` для STRIDE.
 *
 * Джерело — `SBASE` (device-VA), приймач — вторинні атрибути з двійного
 * слова AO. Рядків BLINES, у кожному BSIZE двійних слів, крок між рядками —
 * STRIDE двійних слів.
 */
static void pds_doutd(ClarionPdsCtx *c, const uint32_t *emit, unsigned n)
{
    uint32_t sbase, ctl, bsize, blines, ao, stride, instr, bytes, pa;
    unsigned line, i, copied = 0;

    if (n < PDS_NUM_DMA_CONTROL_WORDS) {
        fprintf(stderr, "[sgx]       DOUTD: операнди не розв'язані\n");
        c->stop = "DOUTD без розв'язаних операндів";
        return;
    }
    sbase = emit[0];
    ctl = emit[1];
    bsize = (ctl & PDS_DOUTD1_BSIZE_MASK) + 1;
    blines = ((ctl >> PDS_DOUTD1_BLINES_SHIFT) & PDS_DOUTD1_BLINES_MASK) + 1;
    ao = (ctl >> PDS_DOUTD1_AO_SHIFT) & PDS_DOUTD1_AO_MASK;
    stride = ((ctl >> PDS_DOUTD1_STRIDE_SHIFT) & PDS_DOUTD1_STRIDE_MASK) + 1;
    instr = (ctl >> PDS_DOUTD1_INSTR_SHIFT) & 3;
    bytes = bsize * blines * 4;

    fprintf(stderr, "[sgx]       DOUTD: SBASE=%08x  DOUTD1=%08x\n", sbase, ctl);
    fprintf(stderr, "[sgx]              BSIZE=%u BLINES=%u AO=%u STRIDE=%u"
            " INSTR=%u STYPE=%u -> %u Б з %08x\n", bsize, blines, ao, stride,
            instr, !!(ctl & PDS_DOUTD1_STYPE), bytes, sbase);
    if (!sgx_translate(c->pd, sbase, &pa)) {
        fprintf(stderr, "[sgx]              ⚠ джерело DMA НЕ відображене в цьому"
                " каталозі\n");
        if (c->exec) {
            c->stop = "джерело DOUTD не відображене";
        }
        return;
    }
    fprintf(stderr, "[sgx]              джерело DMA відображене: PA %08x\n", pa);

    if (!c->exec) {
        return;
    }
    if (instr != PDS_DOUTD1_INSTR_NORMAL) {
        fprintf(stderr, "[sgx]              ⚠ INSTR=%u не NORMAL — не виконуємо\n",
                instr);
        c->stop = "режим DOUTD не NORMAL";
        return;
    }
    if (ao + bsize * blines > SGX_SA_DWORDS) {
        fprintf(stderr, "[sgx]              ⚠ DMA не влазить у банк атрибутів\n");
        c->stop = "DOUTD за межами банку вторинних атрибутів";
        return;
    }

    c->s->sa_sbase = sbase;
    for (line = 0; line < blines; line++) {
        for (i = 0; i < bsize; i++) {
            uint32_t src_va = sbase + 4 * (line * stride + i);
            uint32_t src_pa;

            if (!sgx_translate(c->pd, src_va, &src_pa)) {
                fprintf(stderr, "[sgx]              ⚠ рядок %u слово %u: VA %08x"
                        " не відображений — DMA спинено\n", line, i, src_va);
                c->stop = "розрив у джерелі DOUTD";
                return;
            }
            c->s->sa[ao + line * bsize + i] = sgx_phys_ld32(src_pa);
            c->s->sa_known[ao + line * bsize + i] = true;
            copied++;
        }
    }
    if (ao + copied > c->s->sa_count) {
        c->s->sa_count = ao + copied;
    }
    fprintf(stderr, "[sgx]              ✔ ВИКОНАНО: %u двійних слів -> sa[%u..%u]\n",
            copied, ao, ao + copied - 1);
}

/*
 * DOUTU — запуск задачі USE. У M3-B1 інтерпретатора USE ще немає, тому ми
 * лише називаємо точку входу й кладемо її в чергу: її забере M3-B2.
 * Вигадувати виконання задачі тут було б гірше, ніж не виконувати її зовсім.
 */
static void pds_doutu(ClarionPdsCtx *c, const uint32_t *emit, unsigned n)
{
    uint32_t w0, w1, cbase, coff, exe, exeaddr, va;

    if (n < PDS_NUM_USE_TASK_CONTROL_WORDS) {
        fprintf(stderr, "[sgx]       DOUTU: операнди не розв'язані\n");
        if (c->exec) {
            c->stop = "DOUTU без розв'язаних операндів";
        }
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

    if (!c->exec) {
        return;
    }
    /*
     * PDSDMADEPENDENCY означає «задача чекає на завершення DMA цієї ж
     * програми». Наш DOUTD синхронний і вже відпрацював, тож залежність
     * задоволена за побудовою — але кажемо це вголос, а не мовчки.
     */
    if (c->s->nuse < SGX_USE_QUEUE) {
        c->s->use_queue[c->s->nuse++] = va;
        fprintf(stderr, "[sgx]              ✔ поставлено в чергу задач USE (#%u);"
                " виконання задачі — M3-B2\n", c->s->nuse);
    } else {
        fprintf(stderr, "[sgx]              ⚠ черга задач USE переповнена\n");
    }
}

/*
 * Пройти програму PDS: розібрати кожну інструкцію, а якщо ввімкнено
 * виконання (QY8_SGX_EXEC) — ще й виконати її наслідки.
 *
 * Це один прохід, а не два, навмисно: у трасі видно рівно те, що модель
 * справді зробила. Без QY8_SGX_EXEC поведінка тотожна попередній версії —
 * самий лише розбір, жодного наслідку.
 *
 * Керування потоком справжнє: cc гейтить кожну інструкцію, TSTZ/TSTN ставлять
 * предикат, BRA/CALL/RTN рухають лічильник. Чого не вміємо — не вгадуємо: на
 * першій нетлумаченій інструкції або невідомій умові виконання спиняється з
 * названою причиною.
 */
static void sgx_pds_run(ClarionSgxState *s, uint32_t pd, uint32_t data_va,
                        uint32_t rows, const char *what, bool have_ir0,
                        uint32_t ir0)
{
    ClarionPdsCtx c;
    uint32_t pc = 0, i;
    uint32_t link[8];
    unsigned nlink = 0, steps = 0;
    unsigned n;

    memset(&c, 0, sizeof(c));
    c.s = s;
    c.pd = pd;
    c.data_va = data_va;
    /* Рядок займає PDS_NUM_DWORDS_PER_ROW двійних слів у КОЖНОМУ з двох банків. */
    c.ndwords = rows * 2 * PDS_NUM_DWORDS_PER_ROW;
    c.code_va = data_va + 4 * c.ndwords;
    c.exec = s->exec;
    for (i = 0; i < 16; i++) {
        c.use_base[i] = pds_use_code_base(s, i);
    }
    if (have_ir0) {
        c.ir[0] = ir0;
        c.ir_known[0] = true;
    }

    fprintf(stderr, "[sgx]   %s програми PDS (%s):\n",
            c.exec ? "ВИКОНАННЯ" : "розбір", what);
    fprintf(stderr, "[sgx]     сегмент даних %08x, рядків %u -> %u двійних слів"
            " (%u Б)\n", c.data_va, rows, c.ndwords, 4 * c.ndwords);
    fprintf(stderr, "[sgx]     код з %08x\n", c.code_va);
    if (have_ir0) {
        fprintf(stderr, "[sgx]     ir0 = %08x%s\n", ir0,
                ir0 ? "" : "  (нуль = холодний старт, не відновлення заліза —"
                           " sgx_init.use.asm:93..104)");
    }

    while (pc < 0x100 && steps++ < 256) {
        uint32_t pa, w, group, type, cc;
        bool cc_known, taken;

        if (!sgx_translate(pd, c.code_va + pc, &pa)) {
            fprintf(stderr, "[sgx]     +0x%02x: не відображено — спинено\n", pc);
            c.stop = "код не відображений";
            break;
        }
        w = sgx_phys_ld32(pa);
        group = (w >> PDS_INST_SHIFT) & 3;
        type = (w >> PDS_TYPE_SHIFT) & 7;
        cc = (w >> PDS_CC_SHIFT) & 7;

        fprintf(stderr, "[sgx]     +0x%02x: %08x  група=%u тип=%u cc=%u", pc, w,
                group, type, cc);

        taken = pds_cc_true(&c, cc, &cc_known);
        if (c.exec && !cc_known) {
            fprintf(stderr, "  ⚠ умова невідома — спинено\n");
            c.stop = "невідома умова виконання";
            break;
        }
        if (c.exec && !taken) {
            fprintf(stderr, "  (умова хибна — пропущено)\n");
            pc += PDS_INSTRUCTION_SIZE;
            continue;
        }

        if (group == PDS_INST_FLOW && type == PDS_TYPE_HALT) {
            fprintf(stderr, "  HALT\n");
            break;
        }
        if (group == PDS_INST_FLOW && type == PDS_TYPE_NOP) {
            fprintf(stderr, "  NOP\n");
            pc += PDS_INSTRUCTION_SIZE;
            continue;
        }
        if (group == PDS_INST_FLOW &&
            (type == PDS_TYPE_TSTZ || type == PDS_TYPE_TSTN)) {
            uint32_t dst = w & PDS_TST_DEST_MASK;
            uint32_t src1 = (w >> PDS_TST_SRC1_SHIFT) & PDS_TST_SRC1_MASK;
            uint32_t src2 = (w >> PDS_TST_SRC2_SHIFT) & PDS_TST_SRC2_MASK;
            bool use_src2 = ((w >> PDS_TST_SRCSEL_SHIFT) & 1) ==
                            PDS_TST_SRCSEL_SRC2;
            bool src1_reg = ((w >> PDS_TST_SRC1SEL_SHIFT) & 1) ==
                            PDS_TST_SRC1SEL_REG;
            uint32_t val = 0;
            bool known = false;

            if (use_src2) {
                known = pds_ds_read(&c, 1, src2, &val);
                fprintf(stderr, "  %s p%u, ds1[%u]",
                        type == PDS_TYPE_TSTZ ? "TSTZ" : "TSTN", dst, src2);
            } else if (src1_reg) {
                known = src1 < 2 && c.ir_known[src1];
                val = src1 < 2 ? c.ir[src1] : 0;
                fprintf(stderr, "  %s p%u, ir%u",
                        type == PDS_TYPE_TSTZ ? "TSTZ" : "TSTN", dst, src1);
            } else {
                known = pds_ds_read(&c, 0, src1, &val);
                fprintf(stderr, "  %s p%u, ds0[%u]",
                        type == PDS_TYPE_TSTZ ? "TSTZ" : "TSTN", dst, src1);
            }
            if (known) {
                fprintf(stderr, " = %08x", val);
            }
            if (dst < 3) {
                c.pred_known[dst] = known;
                c.pred[dst] = known &&
                    (type == PDS_TYPE_TSTZ ? val == 0 : (int32_t)val < 0);
                if (known) {
                    fprintf(stderr, " -> p%u=%u", dst, c.pred[dst]);
                }
            }
            if (c.exec && !known) {
                fprintf(stderr, "  ⚠ джерело невідоме — спинено\n");
                c.stop = "джерело TST невідоме";
                break;
            }
            fprintf(stderr, "\n");
            pc += PDS_INSTRUCTION_SIZE;
            continue;
        }
        if (group == PDS_INST_FLOW &&
            (type == PDS_TYPE_BRA || type == PDS_TYPE_CALL)) {
            uint32_t dest = (w & PDS_FLOW_DEST_MASK) << PDS_FLOW_DEST_ALIGNSHIFT;

            fprintf(stderr, "  %s -> інструкція %u (+0x%02x)\n",
                    type == PDS_TYPE_BRA ? "BRA" : "CALL",
                    dest >> PDS_FLOW_DEST_ALIGNSHIFT, dest);
            if (!c.exec) {
                pc += PDS_INSTRUCTION_SIZE;
                continue;
            }
            if (type == PDS_TYPE_CALL) {
                if (nlink >= ARRAY_SIZE(link)) {
                    c.stop = "переповнення стека CALL";
                    break;
                }
                link[nlink++] = pc + PDS_INSTRUCTION_SIZE;
            }
            pc = dest;
            continue;
        }
        if (group == PDS_INST_FLOW && type == PDS_TYPE_RTN) {
            fprintf(stderr, "  RTN\n");
            if (!c.exec) {
                pc += PDS_INSTRUCTION_SIZE;
                continue;
            }
            if (!nlink) {
                c.stop = "RTN без CALL";
                break;
            }
            pc = link[--nlink];
            continue;
        }
        if (group == PDS_INST_MOV && type == PDS_TYPE_MOV32) {
            uint32_t srcsel = (w >> PDS_MOV32_SRCSEL_SHIFT) &
                              PDS_MOV32_SRCSEL_MASK;
            uint32_t src = (w >> PDS_MOV32_SRC_SHIFT) & PDS_MOV32_SRC_MASK;
            uint32_t dstsel = (w >> PDS_MOV32_DESTSEL_SHIFT) &
                              PDS_MOV32_DESTSEL_MASK;
            uint32_t dst = (w >> PDS_MOV32_DEST_SHIFT) & PDS_MOV32_DEST_MASK;
            uint32_t val = 0;
            bool known = false;

            fprintf(stderr, "  MOV32 ds%u[%u] <- ", dstsel, dst);
            if (srcsel == PDS_MOV32_SRCSEL_REG) {
                /*
                 * ir0/ir1 тут закодовані як 0x00/0x02 (`sgxdefs.h:2829`),
                 * а PC і TIM ми не моделюємо й не вдаємо, що моделюємо.
                 */
                if (src == PDS_MOV32_SRC_IR0 || src == PDS_MOV32_SRC_IR1) {
                    unsigned k = src == PDS_MOV32_SRC_IR0 ? 0 : 1;

                    known = c.ir_known[k];
                    val = c.ir[k];
                    fprintf(stderr, "ir%u", k);
                } else {
                    fprintf(stderr, "reg[%u]", src);
                }
            } else {
                known = pds_ds_read(&c, srcsel, src, &val);
                fprintf(stderr, "ds%u[%u]", srcsel, src);
            }
            if (known) {
                fprintf(stderr, " = %08x", val);
                if (!pds_ds_write(&c, dstsel, dst, val) && c.exec) {
                    fprintf(stderr, "  ⚠ приймач не тимчасовий — спинено\n");
                    c.stop = "MOV32 пише в константу";
                    break;
                }
            } else if (c.exec) {
                fprintf(stderr, "  ⚠ джерело невідоме — спинено\n");
                c.stop = "джерело MOV32 невідоме";
                break;
            }
            fprintf(stderr, "\n");
            pc += PDS_INSTRUCTION_SIZE;
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

            /* SRC1 — пара сусідніх значень банку DS0 (або вхідний регістр). */
            if (s1_ds0) {
                have[PDS_MOVS_SWIZ_SRC1L] = pds_ds_read(&c, 0, d1, &pair[0]);
                have[PDS_MOVS_SWIZ_SRC1H] = pds_ds_read(&c, 0, d1 + 1, &pair[1]);
            }
            /* SRC2 — завжди банк DS1: константи або тимчасові. */
            have[PDS_MOVS_SWIZ_SRC2L] = pds_ds_read(&c, 1, d2, &pair[2]);
            have[PDS_MOVS_SWIZ_SRC2H] = pds_ds_read(&c, 1, d2 + 1, &pair[3]);

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
                pds_doutd(&c, emit, n);
            } else if (dest == PDS_MOVS_DEST_DOUTU) {
                pds_doutu(&c, emit, n);
            } else if (c.exec) {
                fprintf(stderr, "[sgx]       ⚠ приймач MOVS %u не тлумачимо —"
                        " спинено\n", dest);
                c.stop = "нетлумачений приймач MOVS";
            }
            if (c.stop) {
                break;
            }
            pc += PDS_INSTRUCTION_SIZE;
            continue;
        }
        fprintf(stderr, "  (не тлумачимо)\n");
        if (c.exec) {
            c.stop = "нетлумачена інструкція";
            break;
        }
        pc += PDS_INSTRUCTION_SIZE;
    }

    if (c.stop) {
        fprintf(stderr, "[sgx]     ⛔ виконання спинено: %s\n", c.stop);
    } else if (steps >= 256) {
        fprintf(stderr, "[sgx]     ⛔ ліміт кроків — спинено\n");
    }
}

/*
 * Звіт про банк вторинних атрибутів після виконання.
 *
 * Тут же — самоперевірка, яка НЕ спирається на жодну зашиту адресу. Розкладка
 * `PVRSRV_SGX_EDMPROG_SECATTR` (`sgx_mkif.h:144..156`) починається полем
 * `sTA3DCtl`, а будівник нашої програми (`sgxinit.c:1961`) кладе в `SBASE`
 * рівно device-VA того самого TA3D-контролю. Отже після правильного DMA
 * `sa[0]` мусить дорівнювати `SBASE`, з якого DMA і читав. Збіг доводить, що
 * зсув, крок і напрямок копіювання правильні; розбіжність одразу це ламає.
 */
static void sgx_report_sa(ClarionSgxState *s)
{
    unsigned i;

    if (!s->sa_count) {
        fprintf(stderr, "[sgx]   вторинні атрибути: DMA не виконувався\n");
        return;
    }
    fprintf(stderr, "[sgx]   вторинні атрибути мікроядра (%u двійних слів):\n",
            s->sa_count);
    for (i = 0; i < s->sa_count; i += 4) {
        unsigned k;

        fprintf(stderr, "[sgx]     sa[%2u]:", i);
        for (k = 0; k < 4 && i + k < s->sa_count; k++) {
            if (s->sa_known[i + k]) {
                fprintf(stderr, " %08x", s->sa[i + k]);
            } else {
                fprintf(stderr, " --------");
            }
        }
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "[sgx]     sa[%u] sTA3DCtl = %08x\n", SGX_SA_TA3DCTL,
            s->sa[SGX_SA_TA3DCTL]);
    fprintf(stderr, "[sgx]     sa[%u] sHostCtl = %08x   ← R_HostCtl коду USE\n",
            SGX_SA_HOSTCTL, s->sa[SGX_SA_HOSTCTL]);
    fprintf(stderr, "[sgx]     sa[%u] sCCBCtl  = %08x\n", SGX_SA_CCBCTL,
            s->sa[SGX_SA_CCBCTL]);
    if (s->sa_known[SGX_SA_TA3DCTL] && s->sa[SGX_SA_TA3DCTL] == s->sa_sbase) {
        fprintf(stderr, "[sgx]     ✔ самоперевірка: sa[0] == SBASE DMA (%08x) —"
                " зсув і крок копіювання правильні\n", s->sa_sbase);
    } else if (s->sa_known[SGX_SA_TA3DCTL]) {
        fprintf(stderr, "[sgx]     ⛔ самоперевірка ПРОВАЛЕНА: sa[0]=%08x, а"
                " SBASE DMA = %08x\n", s->sa[SGX_SA_TA3DCTL], s->sa_sbase);
    }
}

/* --- звіт на kick ----------------------------------------------------- */

static void sgx_report_kick(ClarionSgxState *s)
{
    uint32_t pds   = sgx_reg(s, SGX_CR_PDS_EXEC_BASE) &
                     SGX_PDS_EXEC_BASE_ADDR_MASK;
    uint32_t task  = sgx_reg(s, SGX_CR_EVENT_OTHER_PDS_EXEC);
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
    fprintf(stderr, "[sgx]   EVENT_OTHER_PDS EXEC/DATA/INFO + 0x0A74 ="
            " %08x %08x %08x %08x\n",
            task, sgx_reg(s, SGX_CR_EVENT_OTHER_PDS_DATA),
            sgx_reg(s, SGX_CR_EVENT_OTHER_PDS_INFO), sgx_reg(s, SGX_CR_QY8_TASK_W3));
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
    if (pds && task) {
        /*
         * Програма, на яку показує task-control із MMIO — `sgxinit_primary`
         * (#13). Її ir0 задає залізо на події kick; ми його не знаємо і не
         * вдаємо, що знаємо, — ця програма на ir0 і не дивиться.
         */
        sgx_pds_run(s, pd, root, sgx_reg(s, SGX_CR_EVENT_OTHER_PDS_DATA),
                    "task-control із MMIO", false, 0);
    }

    /*
     * ⚠ Діагностичний важіль, а не ланка чесного ланцюга. Адресу програми
     * інженер бере з траси вище, модель її не шукає й не вгадує. Потрібен,
     * доки немає інтерпретатора USE (M3-B2): без нього `emitpds` із коду
     * мікроядра не може запустити `sgxinit_secondary` (#14), а саме його
     * DOUTD і везе вторинні атрибути.
     */
    if (s->run_set) {
        fprintf(stderr, "[sgx]   ⚠ QY8_SGX_PDS_RUN — діагностичний запуск, не"
                " частина чесного ланцюга\n");
        sgx_pds_run(s, pd, s->run_va, s->run_rows, "QY8_SGX_PDS_RUN",
                    true, s->run_ir0);
    }

    if (s->exec) {
        sgx_report_sa(s);
        if (s->nuse) {
            unsigned k;

            fprintf(stderr, "[sgx]   черга задач USE (виконання — M3-B2):\n");
            for (k = 0; k < s->nuse; k++) {
                fprintf(stderr, "[sgx]     #%u  device VA %08x\n",
                        k, s->use_queue[k]);
            }
        }
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
    memset(s->sa, 0, sizeof(s->sa));
    memset(s->sa_known, 0, sizeof(s->sa_known));
    s->sa_count = 0;
    s->sa_sbase = 0;
    s->nuse = 0;
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

/*
 * QY8_SGX_PDS_RUN="VA[:рядків[:ir0]]" — виконати названу програму PDS.
 *
 * Рядків за замовчуванням 3 (`sgxinit_secondary` має саме стільки: #14
 * = #13 + 0x30, а 3 рядки × 2 банки × 2 дв.сл. = 0x30 Б даних). ir0 за
 * замовчуванням 0 — і це не зручний нуль, а названий випадок: у
 * `sgx_init.use.asm:93..104` ir0 приходить з
 * `HOST_CTL.ui32InterruptClearFlags & PVRSRV_USSE_EDM_INTERRUPT_HWR`, тобто
 * нуль означає «холодний старт, не відновлення заліза», і тільки тоді
 * secondary робить DMA.
 */
static void sgx_parse_pds_run(ClarionSgxState *s, const char *spec)
{
    char *end;

    if (!spec || !*spec) {
        return;
    }
    s->run_va = (uint32_t)strtoull(spec, &end, 0);
    if (end == spec) {
        return;
    }
    s->run_rows = 3;
    s->run_ir0 = 0;
    if (*end == ':') {
        spec = end + 1;
        s->run_rows = (uint32_t)strtoull(spec, &end, 0);
        if (*end == ':') {
            spec = end + 1;
            s->run_ir0 = (uint32_t)strtoull(spec, &end, 0);
        }
    }
    s->run_set = true;
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
    s->exec = getenv("QY8_SGX_EXEC") != NULL;
    sgx_parse_dump(s, getenv("QY8_SGX_DUMP"));
    sgx_parse_find(s, getenv("QY8_SGX_FIND"));
    sgx_parse_pds_run(s, getenv("QY8_SGX_PDS_RUN"));

    if (s->exec) {
        /*
         * M3-B1 виконує програми PDS, але наслідки лишаються всередині
         * моделі: DOUTD наповнює наш банк вторинних атрибутів, DOUTU лише
         * ставить точку входу в чергу. У пам'ять гостя модель не пише і
         * `ui32InitStatus` не чіпає, тож бут A/B і далі мусить збігатися.
         */
        fprintf(stderr, "[sgx] QY8_SGX_EXEC: програми PDS ВИКОНУЮТЬСЯ"
                " (наслідки — лише в стані моделі; гість змін не бачить)\n");
    }
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
