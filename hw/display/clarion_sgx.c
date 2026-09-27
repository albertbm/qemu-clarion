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

typedef struct ClarionSgxDump {
    uint32_t va;
    uint32_t len;
} ClarionSgxDump;

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

static void clarion_sgx_realize(DeviceState *dev, Error **errp)
{
    ClarionSgxState *s = CLARION_SGX(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    const char *e;

    e = getenv("QY8_SGX_KICKS");
    s->kick_reports = e ? (uint32_t)atoi(e) : 2;
    s->readback = getenv("QY8_SGX_READBACK") != NULL;
    sgx_parse_dump(s, getenv("QY8_SGX_DUMP"));

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
