/* Clarion QY7 bootloader machine. */
#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "hw/core/boards.h"
#include "system/block-backend.h"
#include "hw/block/flash.h"
#include "hw/char/clarion_scif.h"
#include "hw/dma/clarion_hpbdma.h"
#include "hw/misc/unimp.h"
#include "hw/misc/clarion_qy7_regs.h"
#include "hw/misc/clarion_usbphy.h"
#include "hw/misc/clarion_dispmicom.h"
#include "hw/misc/clarion_boards.h"
#include "hw/display/clarion_du.h"
#include "hw/display/clarion_2dg.h"
#include "hw/sh4/clarion_sh4_core.h"
#include "hw/sh4/sh.h"
#include "hw/intc/clarion_qy7_int2.h"
#include "hw/timer/tmu012.h"
#include "hw/sd/renesas_sdhi.h"
#include "hw/sd/sd.h"
#include "hw/core/irq.h"
#include "target/sh4/cpu-qom.h"
#include "target/sh4/cpu.h"
#include "qom/object.h"
#include "system/address-spaces.h"
#include "system/reset.h"
#include "system/system.h"

#define QY7_FLASH_SIZE (8 * MiB)
#define QY7_RAM_BASE 0x08000000
/*
 * The kernel's memory table lists regions up to 0x90000000 (P1), i.e.
 * physical 0x08000000..0x10000000: main RAM, then the NK2 image, then a
 * 1 MiB block at 0x0e600000 and a 25 MiB block at 0x0e700000 that
 * KernelIoControl hands to the display drivers.
 */
#define QY7_RAM_SIZE (128 * MiB)
/*
 * TMU input clock. The CE kernel selects the /16 prescaler (TCR0 = 0x21)
 * and counts 2625 ticks per millisecond, so the clock it was built for is
 * 2625000 * 16 Hz. Taken from the guest, not measured on a board.
 */
#define QY7_TMU_CLOCK 42000000
#define QY7_SCIF_BASE 0xffe46000
/*
 * serial_scif.dll keeps a table of eight SCIF channels at 0xffe40000 +
 * 0x1000 * n. The guest maps channels 1, 3, 6 and 7 at boot (VirtualCopy
 * from 0xffe41000, 0xffe43000, 0xffe46000, 0xffe47000).
 */
#define QY7_SCIF1_BASE 0xffe41000
#define QY7_SCIF3_BASE 0xffe43000
#define QY7_SCIF7_BASE 0xffe47000
#define QY7_USBPHY_BASE 0xffe70800
#define QY7_INT2_BASE 0xff804000
#define QY7_DU_BASE 0xfff80000
#define QY7_2DG_BASE 0xffe80000

typedef struct QY7MachineState {
    MachineState parent_obj;
    uint8_t dipsw;
} QY7MachineState;

typedef struct QY7DispSink {
    ClarionScif *scif;
} QY7DispSink;

static int qy7_dispmicom_sink(void *opaque, const uint8_t *buf, int len)
{
    QY7DispSink *sink = opaque;
    int done = 0;

    while (done < len) {
        int room = clarion_scif_can_receive(sink->scif);
        int n;

        if (room <= 0) {
            break;
        }
        n = MIN(room, len - done);
        clarion_scif_receive(sink->scif, buf + done, n);
        done += n;
    }
    return done;
}

static void qy7_machine_instance_init(Object *obj)
{
    QY7MachineState *s = (QY7MachineState *)obj;

    s->dipsw = 7;
}

typedef struct QY7ResetData {
    SuperHCPU *cpu;
} QY7ResetData;

static void qy7_reset(void *opaque)
{
    QY7ResetData *reset = opaque;

    cpu_reset(CPU(reset->cpu));
    reset->cpu->env.pc = 0xa0000000;
}

static const ClarionBoardInfo *qy7_board_by_model(const char *model)
{
    for (int i = 0; i < ARRAY_SIZE(clarion_boards); i++) {
        if (!strcmp(model, clarion_boards[i].model)) {
            return &clarion_boards[i];
        }
    }
    return NULL;
}

static void qy7_report_invalid_model(const char *model)
{
    GString *supported = g_string_new(NULL);

    for (int i = 0; i < ARRAY_SIZE(clarion_boards); i++) {
        if (!strcmp(clarion_boards[i].machine, "clarion-qy7")) {
            g_string_append_printf(supported, "%s%s",
                                   supported->len ? ", " : "",
                                   clarion_boards[i].model);
        }
    }
    error_report("clarion-qy7: flash model %s is not supported; "
                 "supported: %s; use qemu-clarion",
                 model, supported->str);
    g_string_free(supported, true);
}

/*
 * The guest reaches SCIF channels through user mappings (VirtualCopy),
 * whose TLB entries hold a 29-bit physical address: 0xffe41010 arrives as
 * 0x1fe41010 (area 7). Besides the P4 address, each channel therefore
 * needs an alias at A7ADDR(base), as the timer has.
 */
static ClarionScif *qy7_scif_with_alias(MemoryRegion *sysmem, hwaddr base,
                                        const char *name, int index,
                                        qemu_irq irq)
{
    ClarionScif *scif = g_new0(ClarionScif, 1);
    MemoryRegion *alias = g_new0(MemoryRegion, 1);
    g_autofree char *alias_name = g_strdup_printf("%s-a7", name);

    clarion_scif_init(scif, sysmem, base, name, index, index, irq, NULL,
                      false);
    memory_region_init_alias(alias, NULL, alias_name, &scif->mr, 0, 0x100);
    memory_region_add_subregion(sysmem, A7ADDR(base), alias);
    return scif;
}

static void qy7_init(MachineState *machine)
{
    QY7MachineState *qmachine = (QY7MachineState *)machine;
    MemoryRegion *sysmem = get_system_memory();
    MemoryRegion *ram = g_new0(MemoryRegion, 1);
    SuperHCPU *cpu = SUPERH_CPU(cpu_create(machine->cpu_type));
    QY7ResetData *reset;
    ClarionQy7Int2 *int2;
    DeviceState *dispmicom;
    DeviceState *dmac;
    ClarionScif *scif3;
    QY7DispSink *disp_sink;
    DeviceState *du;
    DeviceState *g2d;
    MemoryRegion *du_alias;
    MemoryRegion *g2d_alias;
    MemoryRegion *hpb_chan_alias;
    MemoryRegion *hpb_comm_alias;
    DriveInfo *dinfo;
    char model[CLARION_PROD_MODEL_LEN + 1] = "";
    const ClarionBoardInfo *board;
    bool found = false;

    dinfo = drive_get(IF_PFLASH, 0, 0);
    if (dinfo) {
        BlockBackend *blk = blk_by_legacy_dinfo(dinfo);
        for (int i = 0; i < ARRAY_SIZE(clarion_prod_offsets); i++) {
            uint8_t sig[4];
            uint8_t raw[CLARION_PROD_MODEL_LEN];
            hwaddr off = clarion_prod_offsets[i];

            if (blk_pread(blk, off, sizeof(sig), sig, 0) < 0 ||
                memcmp(sig, "PROD", sizeof(sig)) ||
                blk_pread(blk, off + CLARION_PROD_MODEL_OFF, sizeof(raw),
                         raw, 0) < 0) {
                continue;
            }
            for (int j = 0; j < sizeof(raw); j++) {
                model[j] = g_ascii_isgraph(raw[j]) ? raw[j] : '.';
            }
            model[sizeof(raw)] = '\0';
            found = true;
            break;
        }
    }
    board = found ? qy7_board_by_model(model) : NULL;
    if (!board || strcmp(board->machine, "clarion-qy7")) {
        qy7_report_invalid_model(found ? model : "<no PROD>");
        exit(1);
    }

    if (!cpu) {
        error_report("Unable to create SH7785 CPU");
        exit(1);
    }
    if (qmachine->dipsw > 7) {
        error_report("dipsw must be in the range 0..7");
        exit(1);
    }
    int2 = clarion_qy7_int2_init(sysmem, QY7_INT2_BASE, CPU(cpu),
                                 &cpu->env.intc_handle);
    dmac = qdev_new(TYPE_CLARION_HPBDMA);
    qdev_prop_set_bit(dmac, "qy7-mode", true);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dmac), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dmac), 0, CLARION_HPBDMA_CHAN_BASE);
    sysbus_mmio_map(SYS_BUS_DEVICE(dmac), 1, CLARION_HPBDMA_COMM_BASE);
    hpb_chan_alias = g_new0(MemoryRegion, 1);
    memory_region_init_alias(hpb_chan_alias, NULL, "qy7.hpb-channel-a7",
                             sysbus_mmio_get_region(SYS_BUS_DEVICE(dmac), 0),
                             0, CLARION_HPBDMA_CHAN_SIZE);
    memory_region_add_subregion(sysmem, A7ADDR(CLARION_HPBDMA_CHAN_BASE),
                                hpb_chan_alias);
    hpb_comm_alias = g_new0(MemoryRegion, 1);
    memory_region_init_alias(hpb_comm_alias, NULL, "qy7.hpb-common-a7",
                             sysbus_mmio_get_region(SYS_BUS_DEVICE(dmac), 1),
                             0, CLARION_HPBDMA_COMM_SIZE);
    memory_region_add_subregion(sysmem, A7ADDR(CLARION_HPBDMA_COMM_BASE),
                                hpb_comm_alias);
    for (int i = 0; i < CLARION_HPBDMA_QY7_NUM_IRQ; i++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(dmac), i,
                           clarion_qy7_int2_hpb_channel_irq(int2, i));
    }
    tmu012_init(sysmem, 0xffd80000,
                TMU012_FEAT_TOCR | TMU012_FEAT_3CHAN | TMU012_FEAT_EXTCLK,
                QY7_TMU_CLOCK, clarion_qy7_int2_timer_irq(int2, 0),
                clarion_qy7_int2_timer_irq(int2, 1),
                clarion_qy7_int2_timer_irq(int2, 2), NULL);
    reset = g_new0(QY7ResetData, 1);
    reset->cpu = cpu;
    qemu_register_reset(qy7_reset, reset);
    memory_region_init_ram(ram, NULL, "qy7.ram", QY7_RAM_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, QY7_RAM_BASE, ram);
    clarion_qy7_regs_init(sysmem, qmachine->dipsw);

    pflash_cfi02_register(0, "qy7.flash", QY7_FLASH_SIZE,
                          dinfo ? blk_by_legacy_dinfo(dinfo) : NULL, 64 * KiB,
                          1, 2, 0x0001, 0x227e, 0x2220, 0x2200, 0x555, 0x2aa,
                          0);

    clarion_sh4_core_init(cpu, sysmem);
    du = qdev_new(TYPE_CLARION_DU);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(du), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(du), 0, QY7_DU_BASE);
    du_alias = g_new0(MemoryRegion, 1);
    memory_region_init_alias(du_alias, NULL, "qy7.du-a7",
                             sysbus_mmio_get_region(SYS_BUS_DEVICE(du), 0),
                             0, CLARION_DU_SIZE);
    memory_region_add_subregion(sysmem, A7ADDR(QY7_DU_BASE), du_alias);
    g2d = qdev_new(TYPE_CLARION_2DG);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(g2d), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(g2d), 0, QY7_2DG_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(g2d), 0,
                       clarion_qy7_int2_g2d_irq(int2));
    g2d_alias = g_new0(MemoryRegion, 1);
    memory_region_init_alias(g2d_alias, NULL, "qy7.2dg-a7",
                             sysbus_mmio_get_region(SYS_BUS_DEVICE(g2d), 0),
                             0, 0x200);
    memory_region_add_subregion(sysmem, A7ADDR(QY7_2DG_BASE), g2d_alias);
    clarion_scif_init(g_new0(ClarionScif, 1), sysmem, QY7_SCIF_BASE, "qy7.scif",
                      0, 0, NULL, NULL, false);
    /*
     * SCIF1: the routine at serial_scif.dll+0x8c18 polls SCFSR.TEND
     * (bit 6, offset 0x10) of this channel.
     */
    qy7_scif_with_alias(sysmem, QY7_SCIF1_BASE, "qy7.scif1", 1, NULL);
    /* SCIF3: the same polling routine, SCFSR at VA 0x2b0010. */
    scif3 = qy7_scif_with_alias(sysmem, QY7_SCIF3_BASE, "qy7.scif3", 3, NULL);
    scif3->dmac = dmac;
    scif3->dma_feed = clarion_hpbdma_feed;
    scif3->dma_eod = clarion_hpbdma_eod;
    dispmicom = qdev_new(TYPE_CLARION_DISPMICOM);
    qdev_realize_and_unref(dispmicom, NULL, &error_fatal);
    disp_sink = g_new0(QY7DispSink, 1);
    disp_sink->scif = scif3;
    scif3->dispmicom = dispmicom;
    scif3->dispmicom_tx = clarion_dispmicom_rx_byte;
    clarion_dispmicom_set_sink(dispmicom, qy7_dispmicom_sink, disp_sink);
    /* SCIF7: the same polling routine, SCFSR at VA 0x2f0010. */
    qy7_scif_with_alias(sysmem, QY7_SCIF7_BASE, "qy7.scif7", 7, NULL);
    clarion_usbphy_init(sysmem, QY7_USBPHY_BASE, 2, "qy7.usbphy");

    create_unimplemented_device("qy7.dbsc", 0xfe800000, 0x10000);
    create_unimplemented_device("qy7.lbsc", 0xff800200, 0x200);
    /* 0xff804000..0xff8040ff is the INT2 controller (qy7.int2) */
    create_unimplemented_device("qy7.lbsc-window", 0xff801000, 0x3000);
    create_unimplemented_device("qy7.lbsc-window2", 0xff804100, 0xf00);
    create_unimplemented_device("qy7.gpio", 0xffc40000, 0x4000);
    create_unimplemented_device("qy7.cpg", 0xffc80000, 0x1000);
    create_unimplemented_device("qy7.wdt", 0xffcc0000, 0x1000);
    {
        DeviceState *sdhi = qdev_new(TYPE_RENESAS_SDHI);
        DriveInfo *di = drive_get(IF_SD, 0, 0);
        MemoryRegion *alias = g_new0(MemoryRegion, 1);

        sysbus_realize_and_unref(SYS_BUS_DEVICE(sdhi), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(sdhi), 0, 0xffe4c000);
        sysbus_connect_irq(SYS_BUS_DEVICE(sdhi), 0,
                           clarion_qy7_int2_sdhi_irq(int2, 0));
        memory_region_init_alias(alias, NULL, "qy7.sdhi0-a7",
                                 sysbus_mmio_get_region(
                                     SYS_BUS_DEVICE(sdhi), 0),
                                 0, RENESAS_SDHI_SIZE);
        memory_region_add_subregion(sysmem, A7ADDR(0xffe4c000), alias);
        if (di) {
            DeviceState *card = qdev_new(TYPE_SD_CARD);

            qdev_prop_set_drive_err(card, "drive",
                                   blk_by_legacy_dinfo(di), &error_fatal);
            qdev_realize_and_unref(card, qdev_get_child_bus(sdhi, "sd-bus"),
                                   &error_fatal);
        }
    }
    create_unimplemented_device("qy7.sdhi1", 0xffe4d000, 0x1000);
    create_unimplemented_device("qy7.pfc", 0xfffc0000, 0x10000);
    create_unimplemented_device("qy7.intc", 0xffd00000, 0x1000);
    create_unimplemented_device("qy7.intc-usermask", 0xffd30000, 0x1000);
    create_unimplemented_device("qy7.intc2", 0xffd40000, 0x1000);
}

static void qy7_machine_init(MachineClass *mc)
{
    mc->desc = "Clarion QY7 (SH7785)";
    mc->init = qy7_init;
    mc->default_cpu_type = TYPE_SH7785_CPU;
    mc->ignore_memory_transaction_failures = true;
}

static void qy7_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    qy7_machine_init(mc);
    object_class_property_add_uint8_ptr(
        oc, "dipsw", offsetof(QY7MachineState, dipsw),
        OBJ_PROP_FLAG_READWRITE);
}

static const TypeInfo qy7_machine_typeinfo = {
    .name = MACHINE_TYPE_NAME("clarion-qy7"),
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(QY7MachineState),
    .instance_init = qy7_machine_instance_init,
    .class_init = qy7_machine_class_init,
};

static void qy7_machine_register_types(void)
{
    type_register_static(&qy7_machine_typeinfo);
}

type_init(qy7_machine_register_types)
