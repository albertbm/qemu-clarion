/*
 * PowerVR SGX for the Clarion QY8XXX board: passive MMIO model and BIF/MMU tracer.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_DISPLAY_CLARION_SGX_H
#define HW_DISPLAY_CLARION_SGX_H

#define TYPE_CLARION_SGX "clarion-sgx"

/*
 * Base address comes from a live run, not analogy. `ddi_ncg.dll` maps the
 * block with MmMapIoSpace and stores the kernel pointer in
 * `psDevInfo->pvRegsBaseKM`; a probe captures it at `SGXInitialise` and sees
 * physical base 0xFCE00000.
 *
 * Size 0x4000 is the standard SGX register bank in the DDK. The largest
 * offset observed from the guest is 0x0C84 (`EUR_CR_BIF_DIR_LIST_BASE0`), so
 * the window has ample room and does not overlap any known region.
 */
#define CLARION_SGX_BASE        0xFCE00000
#define CLARION_SGX_SIZE        0x4000

/*
 * Register offsets come from public SGX headers (`sgx531defs.h`; shared by
 * 530/535/540/545 where used). The model does NOT interpret fields except
 * address masks defined in those same headers.
 */
#define SGX_CR_CORE_ID              0x0020
#define SGX_CR_CORE_ID              0x0020
#define SGX_CR_CORE_REVISION        0x0024

/*
 * --- Core identity ------------------------------------------------------
 *
 * `EUR_CR_CORE_REVISION` (`sgx540defs.h:249..261`) is
 * `DESIGNER<<24 | MAJOR<<16 | MINOR<<8 | MAINTENANCE`.
 *
 * VALUE SOURCE. It is neither guessed nor fitted: **the firmware names it**.
 * Handler `HostKickGetMiscInfo` places its `ui32CoreRevSW` constant in its
 * buffer, assembled from three instructions (`eventhandler.use.asm:1138..1141`);
 * the DDK header states that this constant is the
 * constant — "SW core revision converted to internal format (as used in the
 * **EUR_CR_CORE_REVISION** register)» (`sgxdefs.h:145..148`).
 *
 * In our image, these three immediate operands read literally as
 * (GPU VA 0x0E400A40..0x0E400A88):
 *
 *   mov r1, #0x00010000   MAJ = 1
 *   or  r1, r1, #0x00000200   MIN = 2
 *   or  r1, r1, #0x00000000   MAINT = 0      -> 0x00010200, SGX540 r1.2.0
 *
 * The driver requires `ui32CoreRev == ui32CoreRevSW`, otherwise it reports
 * `PVRSRV_ERROR_BUILD_MISMATCH` (`srvkm/.../sgxinit.c:2085`). Its table has
 * one exception, pair (0x10100, 0x10101), unrelated to 0x10200. Since this
 * firmware worked on the real HU, the silicon must have returned 0x00010200.
 */
#define SGX_CORE_REVISION_QY8       0x00010200U

/*
 * ⚠ `EUR_CR_CORE_ID` has NO established source; this is stated explicitly.
 *
 * The microkernel only copies this register to `ui32CoreID`; throughout the
 * DDK this value is used only in debug output (`sgxinit.c:1037`) and returned
 * to callers by `PVRSRVGetSGXRevDataKM` (`sgxutils.c:1609`). No check reads it.
 *
 * Therefore use zero here. This differs from `MNE_CR_EVENT_STATUS`, where
 * zero changes control flow and loops the microkernel forever. A value that
 * the contract never reads can be supplied; behavior-dependent values cannot.
 * The model logs this choice on every read. Override with
 * `QY8_SGX_CORE_ID=` (and `QY8_SGX_CORE_REV=` for the revision).
 */
#define SGX_CORE_ID_UNKNOWN         0x00000000U
#define SGX_CR_SOFT_RESET           0x0080
#define SGX_CR_EVENT_HOST_ENABLE2   0x0110
#define SGX_CR_EVENT_STATUS         0x012C
#define SGX_CR_EVENT_HOST_ENABLE    0x0130
#define SGX_CR_EVENT_HOST_CLEAR     0x0134
#define SGX_CR_EVENT_STATUS_MADD_INVAL 0x04000000U
#define SGX_CR_EVENT_STATUS_TPC_FLUSH  0x00000400U
#define SGX_CR_EVENT_STATUS_TPC_CLEAR  0x00000800U
#define SGX_CR_EVENT_STATUS_SW_EVENT   0x00004000U
#define SGX_CR_EVENT_STATUS_MODELED (SGX_CR_EVENT_STATUS_MADD_INVAL | \
    SGX_CR_EVENT_STATUS_TPC_FLUSH | SGX_CR_EVENT_STATUS_TPC_CLEAR | \
    SGX_CR_EVENT_STATUS_SW_EVENT)
#define SGX_CR_TE_TPCCONTROL       0x0224
#define SGX_CR_TE_TPCCONTROL_FLUSH 0x40000000U
#define SGX_CR_TE_TPCCONTROL_CLEAR 0x80000000U
#define SGX_CR_CACHE_CTRL          0x0804
#define SGX_CR_CACHE_CTRL_INVALIDATE 0x10000000U
#define SGX_CR_USE_CACHE           0x0A08
#define SGX_CR_PDS_CACHE_STATUS     0x0138
#define SGX_CR_PDS_CACHE_HOST_ENABLE 0x013C
#define SGX_CR_PDS_CACHE_HOST_CLEAR 0x0140
#define SGX_CR_PDS_CACHE_STATUS_CSC_INV  0x00000001U
#define SGX_CR_PDS_CACHE_STATUS_DSC_INV0 0x00000002U
#define SGX_CR_PDS_CACHE_STATUS_DSC_INV1 0x00000004U
#define SGX_CR_PDS_CACHE_STATUS_DSC_INV3 0x00000010U
#define SGX_CR_PDS_CACHE_STATUS_MASK     0x00000017U
#define SGX_CR_USE_CODE_BASE(x)     (0x0A0C + 4 * (x))
#define SGX_CR_PDS_EXEC_BASE        0x0AB8
#define SGX_CR_EVENT_KICK2          0x0AC0
#define SGX_CR_EVENT_KICKER         0x0AC4
#define SGX_CR_EVENT_KICK           0x0AC8
#define SGX_CR_EVENT_TIMER          0x0ACC
#define SGX_CR_PDS_INV0             0x0AD0
#define SGX_CR_PDS_INV1             0x0AD4
#define SGX_CR_EVENT_KICK3          0x0AD8
#define SGX_CR_PDS_INV3             0x0ADC
#define SGX_CR_PDS_INV_CSC          0x0AE0
#define SGX_CR_PDS_INV_REQUEST      0x00000001U
#define SGX_CR_BIF_CTRL             0x0C00
#define SGX_CR_BIF_DIR_LIST_BASE0   0x0C84

/* SGX540 DDK sgxfeaturedefs.h: SGX_FEATURE_USE_NUMBER_PC_BITS = 12. */
#define SGX_FEATURE_USE_NUMBER_PC_BITS 12

/*
 * MNE is the system-cache node (`eurasia/hwdefs/mnemedefs.h`). The microkernel
 * uses it whenever the host requests `SGXMKIF_CC_INVAL_BIF_SL`.
 *
 * ⚠ On THIS core the "invalidate all" request is bit 0 of `MNE_CR_CTRL`,
 * not the separate `MNE_CR_CTRL_INVAL` register (0x0D20): `usedefs.h` selects
 * the `#if !defined(MNE_CR_CTRL_INVAL)` branch, confirmed by firmware
 * instructions `ldr 0xD00` -> `or #1` -> `str 0xD00` -> `ldr 0xD14`.
 * Therefore `MNE_CR_CTRL_INVAL_ALL` describes a bit at 0x0D00 although the
 * header associates that mask with register 0x0D20.
 *
 * The protocol is level-triggered: set the request, wait for
 * `EVENT_STATUS.INVAL`, clear it via `EVENT_CLEAR`, then clear the request.
 */
#define SGX_CR_MNE_CTRL             0x0D00
#define SGX_CR_MNE_CTRL_INVAL_ALL   0x00000001U
#define SGX_CR_MNE_EVENT_STATUS     0x0D14
#define SGX_CR_MNE_EVENT_STATUS_INVAL 0x00000001U
#define SGX_CR_MNE_EVENT_CLEAR      0x0D18
#define SGX_CR_MNE_EVENT_CLEAR_INVAL  0x00000001U

/*
 * Task-control triplet for the "Other" event, written LAST by the firmware
 * init script before the kick.
 *
 * ⚠ The previous claim that no public header defined these names and the
 * provisional names `SGX_CR_QY8_TASK_*` were wrong: the names are in
 * `eurasia/hwdefs/sgx540defs.h:2135..2157` in the same DDK tree; we had been
 * checking headers for other cores. Exact offset matches confirm them.
 *
 *   0x0A68 EUR_CR_EVENT_OTHER_PDS_EXEC  ADDR_MASK 0x03FFFFF0 - PDS program
 *          address relative to EUR_CR_PDS_EXEC_BASE (run: 0x0080C180)
 *   0x0A6C EUR_CR_EVENT_OTHER_PDS_DATA  SIZE_MASK 0x3F - data segment size
 *          in 16-byte units (run: 2 -> 32 bytes, matching
 *          "2 rows x 2 banks x 2 dwords")
 *   0x0A70 EUR_CR_EVENT_OTHER_PDS_INFO  DM / ATTRIBUTE_SIZE / USESECEXEC
 *
 * The microkernel writes the same triplet from USE using `str #666/#667`
 * (register number = byte offset / 4).
 */
#define SGX_CR_EVENT_PDS_ENABLE     0x0A58
#define SGX_CR_EVENT_OTHER_PDS_EXEC 0x0A68
#define SGX_CR_EVENT_OTHER_PDS_DATA 0x0A6C
#define SGX_CR_EVENT_OTHER_PDS_INFO 0x0A70
#define SGX_CR_QY8_TASK_W3          0x0A74

#define SGX_EVENT_OTHER_PDS_EXEC_ADDR_MASK  0x03FFFFF0U
#define SGX_EVENT_OTHER_PDS_DATA_SIZE_MASK  0x0000003FU

/* Address field masks, copied from sgx*defs.h. */
#define SGX_PDS_EXEC_BASE_ADDR_MASK     0x0FF00000U
#define SGX_EVENT_KICKER_ADDR_MASK      0x0FFFFFF0U
#define SGX_USE_CODE_BASE_ADDR_MASK     0x00FFFFFFU
#define SGX_USE_CODE_BASE_DM_MASK       0x03000000U
#define SGX_USE_CODE_BASE_DM_SHIFT      24
#define SGX_BIF_DIR_LIST_BASE_ADDR_MASK 0xFFFFF000U

#define SGX_CR_EVENT_KICK2_NOW          0x00000001U

/*
 * Software event. ⚠ Numbering DIFFERS between two spaces and is easy to mix up:
 *
 *   registers  `EUR_CR_EVENT_{STATUS,HOST_ENABLE,HOST_CLEAR,PDS_ENABLE}`
 *              - `SW_EVENT` is bit **14** (`sgx540defs.h:536,631,726,2043`);
 *   PDS input  `ir1` - `EURASIA_PDS_IR1_EDM_EVENT_SWEVENT` is bit **8**
 *              (`sgxdefs.h:3566`, the non-543/544/554 branch used here).
 *
 * Map them by the shared event name, not by bit position.
 */
#define SGX_EVENT_SW_EVENT_MASK         0x00004000U
#define PDS_IR1_EDM_EVENT_SWEVENT       (1u << 8)
#define PDS_IR1_EDM_EVENT_KICKPTR_SHIFT 24

/*
 * SGX MMU format from DDK 1.7 services4/srvkm/hwdefs/sgxmmu.h, without
 * SGX_FEATURE_36BIT_MMU: device VA = 10-bit PDE + 10-bit PTE + 12-bit offset,
 * with 1024 entries in both directory and table, and bit 0 = valid.
 */
#define SGX_MMU_PAGE_SHIFT      12
#define SGX_MMU_PAGE_SIZE       (1u << SGX_MMU_PAGE_SHIFT)
#define SGX_MMU_PD_SHIFT        22
#define SGX_MMU_ENTRIES         1024
#define SGX_MMU_ENTRY_VALID     0x00000001U
#define SGX_MMU_ENTRY_ADDR_MASK 0xFFFFF000U

/*
 * --- PDS encoding (SGX540) ---------------------------------------------
 *
 * Everything below follows the public DDK SGX540 branch (without
 * `SGX_FEATURE_USE_UNLIMITED_PHASES` and without `SGX_FEATURE_PDS_EXTENDED_SOURCES`):
 * `eurasia/hwdefs/sgxdefs.h` and `eurasia/codegen/pds/pds.c` in GFX_Linux_DDK
 * @ f184ac914561fa100a5c92a488df777de8785f93. Nothing is guessed.
 */
#define PDS_INSTRUCTION_SIZE        4
#define PDS_INST_SHIFT              30      /* bits 31:30: group */
#define PDS_INST_MOV                0
#define PDS_INST_ARITH              1
#define PDS_INST_FLOW               2
#define PDS_INST_LOGIC              3
#define PDS_TYPE_SHIFT              27      /* bits 29:27: type */
#define PDS_TYPE_MOVS               0
#define PDS_TYPE_MOV16              1
#define PDS_TYPE_MOV32              2
#define PDS_TYPE_TSTZ               0       /* in FLOW group */
#define PDS_TYPE_BRA                2
#define PDS_TYPE_HALT               5
#define PDS_TYPE_TSTN               1       /* in FLOW group */
#define PDS_TYPE_CALL               3
#define PDS_TYPE_RTN                4
#define PDS_TYPE_NOP                6
#define PDS_CC_SHIFT                24      /* bits 26:24 */
#define PDS_CC_P0                   0
#define PDS_CC_P1                   1
#define PDS_CC_P2                   2
#define PDS_CC_IF0                  3
#define PDS_CC_IF1                  4
#define PDS_CC_ALUZ                 5
#define PDS_CC_ALUN                 6
#define PDS_CC_ALWAYS               7

/*
 * Logical operations use `sgxdefs.h:3119..3176`, the branch WITHOUT
 * `SGX_FEATURE_PDS_EXTENDED_SOURCES` (SGX540): no `SRC2SEL`; source 2 is
 * always DS1 bank, and `SRC2` is at bit 10, not bit 9.
 */
#define PDS_TYPE_OR                 0       /* in LOGIC group */
#define PDS_TYPE_AND                1
#define PDS_TYPE_XOR                2
#define PDS_TYPE_NOT                3
#define PDS_TYPE_NOR                4
#define PDS_TYPE_NAND               5
#define PDS_TYPE_SHL                6
#define PDS_TYPE_SHR                7
#define PDS_LOGIC_SRC1SEL_SHIFT     23
#define PDS_LOGIC_SRC1SEL_REG       1
#define PDS_LOGIC_SRC1_SHIFT        17
#define PDS_LOGIC_SRC1_MASK         0x3F
#define PDS_LOGIC_SRC2_SHIFT        10
#define PDS_LOGIC_SRC2_MASK         0x3F
#define PDS_LOGIC_DESTSEL_SHIFT     6
#define PDS_LOGIC_DESTSEL_MASK      0x1
#define PDS_LOGIC_DEST_MASK         0x3F
#define PDS_LOGIC_SRC1_IR0          0x00
#define PDS_LOGIC_SRC1_IR1          0x01
#define PDS_LOGIC_SRC1_TIM          0x02

/*
 * TSTZ/TSTN use `sgxdefs.h:3068..3090`. Predicate p0..p2 (bits 2:0) gets
 * the result of testing one of two sources against zero; SRCSEL (bit 8)
 * selects the source. SRC1 is ds0[] or input register ir0/ir1; SRC2 is ds1[].
 */
#define PDS_TST_DEST_MASK           0x7
#define PDS_TST_SRC1SEL_SHIFT       23
#define PDS_TST_SRC1SEL_REG         1
#define PDS_TST_SRC1_SHIFT          17
#define PDS_TST_SRC1_MASK           0x3F
#define PDS_TST_SRC2_SHIFT          10
#define PDS_TST_SRC2_MASK           0x3F
#define PDS_TST_SRCSEL_SHIFT        8
#define PDS_TST_SRCSEL_SRC2         1
#define PDS_TST_SRC1_IR0            0x00
#define PDS_TST_SRC1_IR1            0x01

/*
 * BRA/CALL — `sgxdefs.h:3106..3109` (`FLOW_DEST` bits 18:0).
 *
 * ⚠ It is easy to get the shift wrong here; this has happened before.
 * `pdsasm/main.c:4736` computes `uDest = uLabelOffset << ALIGNSHIFT`, while
 * `PDSEncodeBRA` (`sgxpdsdefs.h:775..782`) stores `uDest >> ALIGNSHIFT` in the field.
 * The shifts cancel, so the field contains the **instruction number**; the
 * byte offset from the start of the code segment is field << ALIGNSHIFT.
 */
#define PDS_FLOW_DEST_MASK          0x7FFFF
#define PDS_FLOW_DEST_ALIGNSHIFT    2

/*
 * MOVS without `SGX_FEATURE_PDS_EXTENDED_SOURCES` (our SGX540 variant).
 * ⚠ This differs from the extended variant: there is no `SRC2SEL` (source 2
 * always uses bank DS1), `SRC2` is at bit 13, and swizzles are at 11/9/7/5,
 * not 10/8/6/4. `SRC1`/`SRC2` index QUADWORDS, so the doubleword index is
 * `src * PDS_NUM_DWORDS_PER_QWORD + (swiz & 1)` (`pdsdisasm.c`).
 */
#define PDS_MOVS_SRC1SEL_SHIFT      23
#define PDS_MOVS_SRC1SEL_REG        1
/*
 * When SRC1SEL = REG, SRC1 indexes the task's input registers rather than
 * the datastore (`sgxdefs.h:2674..2676`).
 */
#define PDS_MOVS_SRC1_IR0           0x00
#define PDS_MOVS_SRC1_IR1           0x01
#define PDS_MOVS_SRC1_TIM           0x02
#define PDS_MOVS_SRC1_SHIFT         18
#define PDS_MOVS_SRC1_MASK          0x1F
#define PDS_MOVS_SRC2_SHIFT         13
#define PDS_MOVS_SRC2_MASK          0x1F
#define PDS_MOVS_SWIZ_SHIFT(i)      (11 - 2 * (i))   /* SWIZ0..SWIZ3 */
#define PDS_NUM_DWORDS_PER_QWORD    2
#define PDS_DATASTORE_TEMPSTART     48
#define PDS_MOVS_DEST_MR            0
#define PDS_MOVS_SWIZ_SRC1L         0
#define PDS_MOVS_SWIZ_SRC1H         1
#define PDS_MOVS_SWIZ_SRC2L         2
#define PDS_MOVS_SWIZ_SRC2H         3
#define PDS_MOVS_DEST_MASK          0xF
#define PDS_MOVS_DEST_SLC           1
#define PDS_MOVS_DEST_DOUTI         2
#define PDS_MOVS_DEST_DOUTD         3
#define PDS_MOVS_DEST_DOUTT         4
#define PDS_MOVS_DEST_DOUTU         5
#define PDS_MOVS_DEST_DOUTA         6

/* MOV32 — `sgxdefs.h:2819..2845`. */
#define PDS_MOV32_SRCSEL_SHIFT      15
#define PDS_MOV32_SRCSEL_MASK       0x3
#define PDS_MOV32_SRCSEL_DS0        0
#define PDS_MOV32_SRCSEL_DS1        1
#define PDS_MOV32_SRCSEL_REG        2
#define PDS_MOV32_SRC_SHIFT         9
#define PDS_MOV32_SRC_MASK          0x3F
#define PDS_MOV32_DESTSEL_SHIFT     7
#define PDS_MOV32_DESTSEL_MASK      0x1
#define PDS_MOV32_DESTSEL_DS0       0
#define PDS_MOV32_DESTSEL_DS1       1
#define PDS_MOV32_DEST_SHIFT        1
#define PDS_MOV32_DEST_MASK         0x3F
#define PDS_MOV32_SRC_IR0           0x00
#define PDS_MOV32_SRC_IR1           0x02
#define PDS_MOV32_SRC_PC            0x04
#define PDS_MOV32_SRC_TIM           0x06

/* Datastore bank size, `sgxdefs.h:2485..2491`: 0..47 constants, 48..63 temps. */
#define PDS_DATASTORE_PERBANKSIZE   64

/* Data segment layout: PDS_NUM_DWORDS_PER_ROW = 2 on SGX540. */
#define PDS_NUM_DWORDS_PER_ROW      2
#define PDS_NUM_USE_TASK_CONTROL_WORDS  3
#define PDS_NUM_DMA_CONTROL_WORDS   2

/* DOUTD — PDS DMA interface */
#define PDS_DOUTD1_BSIZE_MASK       0xF
#define PDS_DOUTD1_BLINES_SHIFT     4
#define PDS_DOUTD1_BLINES_MASK      0xF
#define PDS_DOUTD1_AO_SHIFT         8
#define PDS_DOUTD1_AO_MASK          0x7FF
#define PDS_DOUTD1_INSTR_SHIFT      19
#define PDS_DOUTD1_STRIDE_SHIFT     21
#define PDS_DOUTD1_STRIDE_MASK      0x1FF
#define PDS_DOUTD1_STYPE            (1u << 30)
#define PDS_DOUTD1_INSTR_NORMAL     0
#define PDS_DOUTD1_INSTR_BYPASS     1
#define PDS_DOUTD1_INSTR_LINEFILL   2

/*
 * Microkernel secondary attributes. Their layout is defined by
 * `PVRSRV_SGX_EDMPROG_SECATTR` (`services4/srvinit/devices/sgx/sgx_mkif.h:144..156`):
 * doubleword 0 is `sTA3DCtl`, 1 is `sHostCtl`, 2 is `sCCBCtl`, followed by
 * `sMKState`. `usedefs.h:68..72` gives the names used by USE code:
 * `R_HostCtl = SA(sHostCtl)`, exactly `sa[1]` in the microkernel listing.
 *
 * `SGX_UKERNEL_SA_BURST_SIZE = 16` doublewords (`sgx_mkif.h:171`), and the
 * total size is `SGX_UKERNEL_NUM_SEC_ATTRIB` (`sgx_mkif.h:174`). In our build,
 * DOUTD transfers 2 rows of 16, or 32 doublewords. The bank includes margin
 * up to the AO field maximum.
 */
#define SGX_SA_DWORDS               (PDS_DOUTD1_AO_MASK + 1)
#define SGX_SA_TA3DCTL              0
#define SGX_SA_HOSTCTL              1
#define SGX_SA_CCBCTL               2

/*
 * DOUTA writes to the USE task's PRIMARY attributes (`sgxdefs.h:3654..3662`).
 * Word 0 contains the data (all 32 bits); word 1 carries `AO`, the offset in
 * the attribute bank, bits 18:8 in our variant (CLRMSK `0xFFF800FF`).
 *
 * The microkernel event handler starts this way:
 * `movs douta, ir0, INPUT_IR0_PA_DEST` and likewise for `ir1`
 * (`services4/srvinit/devices/sgx/eventhandler.pds.asm:84..85`): it transfers
 * event flags from input registers into attributes so USE code can check
 * which event occurred.
 */
#define PDS_NUM_ATTRIB_CONTROL_WORDS 2
#define PDS_DOUTA1_AO_SHIFT         8
#define PDS_DOUTA1_AO_MASK          0x7FF

/* Primary attribute bank; size follows the AO field maximum. */
#define SGX_PA_DWORDS               (PDS_DOUTA1_AO_MASK + 1)

/* DOUTU: USE task launch interface */
#define PDS_DOUTU0_CBASE_MASK       0xF
#define PDS_DOUTU0_COFF_SHIFT       4
#define PDS_DOUTU0_COFF_MASK        0xF
#define PDS_DOUTU0_COFF_ALIGNSHIFT  15
#define PDS_DOUTU0_EXE_SHIFT        8
#define PDS_DOUTU0_EXE_MASK         0x7FF
#define PDS_DOUTU0_EXE_ALIGNSHIFT   4
#define PDS_DOUTU0_ITERATORSDEP     (1u << 19)
#define PDS_DOUTU0_TEXTUREDEP       (1u << 20)
#define PDS_DOUTU0_PDSDMADEP        (1u << 21)
#define PDS_DOUTU1_MODE_SHIFT       26

/*
 * --- USE encoding (SGX540) ---------------------------------------------
 *
 * Directly from `eurasia/hwdefs/sgxdefs.h` in the same DDK tree. An
 * instruction is a pair of little-endian 32-bit words: word0 (low) and word1
 * (high). The opcode is in word1, bits 31:27.
 *
 * ⚠ Bit 18 of word1 is CONTEXT-DEPENDENT: `END` (end task) for instructions
 * without S0 operand, `S0BEXT` for LD/ST, and `PARTIAL` for TEST. Do not test
 * it blindly for every opcode.
 */
#define USE_INST_SIZE               8

#define USE1_OP_SHIFT               27
#define USE1_OP_MOVC                5
#define USE1_OP_TEST                9
#define USE1_OP_ANDOR               10
#define USE1_OP_XOR                 11
#define USE1_OP_SHLROL              12
#define USE1_OP_SHRASR              13
#define USE1_OP_IMAE                21
#define USE1_OP_LD                  29
#define USE1_OP_ST                  30
#define USE1_OP_SPECIAL             31

/* Extended predicate (bits 26:24), `sgxdefs.h:5166..5176`. */
#define USE1_EPRED_SHIFT            24
#define USE1_EPRED_MASK             0x7
#define USE1_EPRED_ALWAYS           0
#define USE1_EPRED_P0               1
#define USE1_EPRED_P1               2
#define USE1_EPRED_P2               3
#define USE1_EPRED_P3               4
#define USE1_EPRED_NOTP0            5
#define USE1_EPRED_NOTP1            6
#define USE1_EPRED_PNMOD4           7

/*
 * Short predicate (bits 26:25), `sgxdefs.h:5184..5190`. Integer instructions
 * (IMAE and the rest of the INT group) carry their predicate HERE, not in
 * EPRED: bit 27 holds the opcode, leaving 26:25 for the predicate. Keep the
 * fields separate to avoid confusing these encodings.
 */
#define USE1_SPRED_SHIFT            25
#define USE1_SPRED_MASK             0x3
#define USE1_SPRED_ALWAYS           0
#define USE1_SPRED_P0               1
#define USE1_SPRED_P1               2
#define USE1_SPRED_NOTP0            3

/* word1 flags, `sgxdefs.h:5198..5206`. */
#define USE1_END                    0x00040000U
#define USE1_S0BEXT                 0x00040000U
#define USE1_S1BEXT                 0x00020000U
#define USE1_S2BEXT                 0x00010000U
#define USE1_DBEXT                  0x00080000U

/* Register fields, `sgxdefs.h:5225..5305`. */
#define USE1_S0BANK_SHIFT           2
#define USE1_S0BANK_MASK            0x1
#define USE1_D1BANK_SHIFT           0
#define USE1_D1BANK_MASK            0x3
#define USE0_S1BANK_SHIFT           30
#define USE0_S2BANK_SHIFT           28
#define USE0_BANK_MASK              0x3
#define USE0_DST_SHIFT              21
#define USE0_SRC0_SHIFT             14
#define USE0_SRC1_SHIFT             7
#define USE0_SRC2_SHIFT             0
#define USE0_REG_MASK               0x7F

/* Banks: Std when the corresponding *BEXT is clear; Ext when set. */
#define USE_S0STDBANK_TEMP          0
#define USE_S0STDBANK_PRIMATTR      1
#define USE_S0EXTBANK_OUTPUT        0
#define USE_S0EXTBANK_SECATTR       1
#define USE_S12STDBANK_TEMP         0
#define USE_S12STDBANK_OUTPUT       1
#define USE_S12STDBANK_PRIMATTR     2
#define USE_S12STDBANK_SECATTR      3
#define USE_S12EXTBANK_INDEXED      0
#define USE_S12EXTBANK_SPECIAL      1
#define USE_S12EXTBANK_IMMEDIATE    2
#define USE_S12EXTBANK_FPINTERNAL   3
#define USE_D1STDBANK_TEMP          0
#define USE_D1STDBANK_OUTPUT        1
#define USE_D1STDBANK_PRIMATTR      2
#define USE_D1EXTBANK_SECATTR       0

/*
 * Indexed addressing. A `mov` destination in `D1EXTBANK_INDEX`
 * (`sgxdefs.h:5260`) stores a MASK, not a register number, in the index field:
 * the reference assembler accepts only 1, 2, 3 and rejects other values with
 * "Invalid index mask" (`useasm.c:1920..1927`). An indexed operand
 * (`S12EXTBANK_INDEXED` source, `D1STDBANK_INDEXED` destination) encodes
 * {bank, register selection, offset}: bits 6:5 bank, bit 4 IDXSEL, bits 3:0
 * offset (`sgxdefs.h:7719..7733`; assembled in `useasm.c:1023..1060`).
 */
#define USE_D1STDBANK_INDEXED       3
#define USE_D1EXTBANK_INDEX         2
#define USE_INDEX_BANK_SHIFT        5
#define USE_INDEX_BANK_MASK         0x3
#define USE_INDEX_IDXSEL            0x10U
#define USE_INDEX_OFFSET_MASK       0xFU
#define USE_INDEX_BANK_TEMP         0
#define USE_INDEX_BANK_OUTPUT       1
#define USE_INDEX_BANK_PRIMATTR     2
#define USE_INDEX_BANK_SECATTR      3
#define USE_INDEX_MASK_L            1
#define USE_INDEX_MASK_H            2
#define USE_INDEX_BANK_SIZE         2

/* SPECIAL: category (bits 21:20) — `sgxdefs.h:6453..6459`. */
#define USE1_SPECIAL_OPCAT_SHIFT    20
#define USE1_SPECIAL_OPCAT_MASK     0x3
#define USE1_SPECIAL_OPCAT_FLOWCTRL 0
#define USE1_SPECIAL_OPCAT_MOECTRL  1
#define USE1_SPECIAL_OPCAT_OTHER    2
#define USE1_SPECIAL_OPCAT_VISTEST  3

/* FLOWCTRL: sub-opcode (bits 8:6) — `sgxdefs.h:6496..6507`. */
#define USE1_FLOWCTRL_OP2_SHIFT     6
#define USE1_FLOWCTRL_OP2_MASK      0x7
#define USE1_FLOWCTRL_OP2_BA        0
#define USE1_FLOWCTRL_OP2_BR        1
#define USE1_FLOWCTRL_OP2_LAPC      2
#define USE1_FLOWCTRL_OP2_SETL      3
#define USE1_FLOWCTRL_OP2_SAVL      4
#define USE1_FLOWCTRL_OP2_NOP       5

/*
 * `SETL` and `SAVL` encode `mov pclink, src1` and `mov dst, pclink`
 * (`useasm.c:14077..14120`): the assembler has no separate instruction to
 * write the link register; it uses a special `mov` form. SETL reads SRC1,
 * while SAVL uses the normal destination field.
 *
 * ⚠ The link register holds an instruction number relative to the code
 * window, not an address. The microkernel returns host-kick handlers to the
 * host as `ui32ServiceAddress = <label offset> / EURASIA_USE_INSTRUCTION_SIZE`
 * (`srvinit/devices/sgx/sgxinit.c:690..692`); this value enters `pclink`
 * unchanged. It uses the same address space as the `ba` target.
 */

/* Branch: `sgxdefs.h:6518..6521`. The word0 offset is a PAIR number, step 8 B. */
#define USE1_BRANCH_SAVELINK        0x00000200U
#define USE0_BRANCH_OFFSET_MASK     0x00000FFFU
#define USE1_FLOWCTRL_NOSCHED       0x00000800U
#define USE1_FLOWCTRL_SYNCEND       0x00800000U
#define USE1_FLOWCTRL_SYNCEXT       0x00001000U
#define USE1_FLOWCTRL_EXCEPTION    0x00080000U
#define USE1_BRANCH_MONITOR         0x00000400U
#define USE1_BRANCH_MODIFIER_MASK   (USE1_BRANCH_SAVELINK | \
                                     USE1_BRANCH_MONITOR | \
                                     USE1_FLOWCTRL_NOSCHED | \
                                     USE1_FLOWCTRL_SYNCEXT | \
                                     USE1_FLOWCTRL_EXCEPTION | \
                                     USE1_FLOWCTRL_SYNCEND)

/* OTHER: sub-opcode (bits 26:24) — `sgxdefs.h:6656..6666`. */
#define USE1_OTHER_OP2_SHIFT        24
#define USE1_OTHER_OP2_MASK         0x7
#define USE1_OTHER_OP2_IDF          0
#define USE1_OTHER_OP2_WDF          1
#define USE1_OTHER_OP2_EMIT         3
#define USE1_OTHER_OP2_LIMM         4
#define USE1_OTHER_OP2_LOCKRELEASE  5
#define USE1_OTHER_OP2_LDRSTR       6
#define USE1_OTHER_OP2_WOP          7

/* MOECTRL: sub-opcode (bits 26:24) — `sgxdefs.h:6553..6560`. */
#define USE1_MOECTRL_OP2_SHIFT      24
#define USE1_MOECTRL_OP2_MASK       0x7
#define USE1_MOECTRL_OP2_SMLSI      2

/*
 * LIMM — `sgxdefs.h:7435..7447`. The 32-bit constant is split into three
 * parts: bits 20:0 in word0, bits 25:21 in word1[8:4], and bits 31:26 in
 * word1[17:12].
 */
#define USE0_LIMM_IMML21_MASK       0x001FFFFFU
#define USE1_LIMM_IMM2521_SHIFT     4
#define USE1_LIMM_IMM2521_MASK      0x1F
#define USE1_LIMM_IMM3126_SHIFT     12
#define USE1_LIMM_IMM3126_MASK      0x3F

/*
 * The LIMM predicate has its own field (bits 11:9, `sgxdefs.h:7443..7444`),
 * with the same values as regular EPRED: the reference assembler puts the
 * result of the same `EncodePredicate(..., FALSE)` there
 * (`useasm.c:14012`, `useasm.c:2664..2671`).
 */
#define USE1_LIMM_EPRED_SHIFT       9
#define USE1_LIMM_EPRED_MASK        0x7

/*
 * LDRSTR (`str`/`ldr`) — `sgxdefs.h:7461..7475`. The special-register number
 * combines SRC2 (bits 6:0) and SRC2EXT (bits 20:14), shifted by 7. Per T6,
 * number × 4 is the byte offset of the SGX register.
 */
#define USE1_LDRSTR_DSEL_STORE      0x00080000U
#define USE0_LDRSTR_SRC2EXT_SHIFT   14
#define USE0_LDRSTR_SRC2EXT_MASK    0x7F
#define USE_LDRSTR_SRC2EXT_INTSHIFT 7

/* LD/ST — `sgxdefs.h:6374..6416`. */
#define USE1_LDST_BPCACHE           0x00080000U
#define USE1_LDST_DTYPE_SHIFT       4
#define USE1_LDST_DTYPE_MASK        0x3
#define USE1_LDST_DTYPE_32BIT       0
#define USE1_LDST_DTYPE_16BIT       1
#define USE1_LDST_DTYPE_8BIT        2
#define USE1_LDST_AMODE_SHIFT       10
#define USE1_LDST_AMODE_MASK        0x3
#define USE1_LDST_AMODE_ABSOLUTE    0
#define USE1_LDST_IMODE_SHIFT       8
#define USE1_LDST_IMODE_MASK        0x3
#define USE1_LDST_IMODE_NONE        0
#define USE1_LDST_IMODE_PRE         1
#define USE1_LDST_IMODE_POST        2
#define USE1_LDST_IMODE_RESERVED    3

/*
 * The LD destination bank is a separate one-bit flag, not the D1BANK field
 * (`sgxdefs.h:6397..6399`). The assembler explicitly forbids any other bank:
 * «The destination for an LD must be the primary attribute bank or the
 * temporary bank» (`useasm.c:12266`).
 */
#define USE1_LDST_DBANK_PRIMATTR    0x00000080U

/*
 * ⚠ `MOEEXPAND` is an INVERTED fetch-mode flag (`sgxdefs.h:6376`).
 * Reference decoder: `if (!(uInst1 & MOEEXPAND)) FETCHENABLE`
 * (`usedisasm.c:2286..2288`). Thus:
 *
 *   MOEEXPAND = 1 → normal access; MOE expands repeats;
 *   MOEEXPAND = 0 → FETCH mode (`.fetchN`): N consecutive doublewords into N
 *                   consecutive destination registers; MOE is not involved.
 *
 * Both modes use the same counter, `RMSKCNT`, whose value is one less than N
 * (`useasm.c:12176`: `(uRptCount - 1) << RMSKCNT_SHIFT`;
 * `usedisasm.c:2306`: `uMaskCount + 1`).
 */
#define USE1_LDST_MOEEXPAND         0x00200000U
#define USE1_RMSKCNT_SHIFT          12
#define USE1_RMSKCNT_MASK           0xF

/*
 * `.fcfill` = force cache line fill (`sgxdefs.h:6413`), a data-cache hint.
 * The model has no cache, so this requirement is trivially satisfied.
 */
#define USE1_LDST_FCLFILL           0x00000002U
#define USE1_LDST_RANGEENABLE       0x00000040U
#define USE1_LDST_INCSGN            0x00000008U

/*
 * IMAE is integer multiply-add, `sgxdefs.h:5155, 6021..6055`.
 * Its semantics come from the IMG compiler comment (`usc2/finalise.c:3137`):
 *
 *   IMAE  DST, SRC0, #SRC1, SRC2   //  DST = SRC0 * SRC1 + SRC2
 *
 * The multipliers are HALFWORDS (16 bits); SRC0H/SRC1H select which half.
 * SRC2 has its own type (16-bit zero-extended, 16-bit sign-extended, or full
 * 32-bit). The `iaddu32 d, a, b` pseudo-instruction is the same IMAE with
 * SRC1 = #1 (`useasm.c:6217..6252`), so it needs no separate implementation.
 */
#define USE1_IMAE_SRC0H_SELECTHIGH  0x01000000U
#define USE1_IMAE_SRC1H_SELECTHIGH  0x00200000U
#define USE1_IMAE_SRC2H_SELECTHIGH  0x00100000U
#define USE1_IMAE_SIGNED            0x00000800U
#define USE1_IMAE_SATURATE          0x00000400U
#define USE1_IMAE_CARRYINENABLE     0x00000200U
#define USE1_IMAE_CARRYOUTENABLE    0x00000100U
#define USE1_IMAE_SRC2TYPE_SHIFT    6
#define USE1_IMAE_SRC2TYPE_MASK     0x3
#define USE1_IMAE_SRC2TYPE_16BITZEXT 0
#define USE1_IMAE_SRC2TYPE_16BITSEXT 1
#define USE1_IMAE_SRC2TYPE_32BIT    2
#define USE1_IMAE_ORSHIFT_SHIFT     3
#define USE1_IMAE_ORSHIFT_MASK      0x7

/* Integer group repeat counter — `sgxdefs.h:5641..5643`. */
#define USE1_INT_RCOUNT_SHIFT       12
#define USE1_INT_RCOUNT_MASK        0x7

/* TEST — `sgxdefs.h:5488..5587`. */
#define USE1_TEST_ZTST_SHIFT        8
#define USE1_TEST_ZTST_MASK         0x3
#define USE1_TEST_ZTST_NONE         0
#define USE1_TEST_ZTST_ZERO         1
#define USE1_TEST_ZTST_NOTZERO      2
#define USE1_TEST_ZTST_RESERVED     3
#define USE1_TEST_STST_SHIFT        10
#define USE1_TEST_STST_MASK         0x3
#define USE1_TEST_STST_NONE         0
#define USE1_TEST_STST_NEGATIVE     1
#define USE1_TEST_STST_POSITIVE     2
#define USE1_TEST_STST_RESERVED     3

/*
 * ⚠ `TEST` has TWO independent checks, for zero and sign; a separate bit
 * selects how they combine: `CRCOMB` set = AND, clear = OR
 * (`sgxdefs.h:5509`, `usedisasm.c:9385`). `NONE` in either field does not
 * mean "no check"; it means "check always true"
 * (`usedisasm.c:9222..9232`: `STST_NONE -> SIGN_TRUE`,
 * `ZTST_NONE -> ZERO_TRUE`), so with AND it does not affect the result.
 *
 * `POSITIVE` means "sign bit CLEAR", not "> 0". This is clear from the
 * assembler itself (`use.l:761..767`), where the suffixes expand literally:
 *
 *   .tests  -> n&t    sign bit set
 *   .testns -> p&t    sign bit clear      <- our case
 *   .testp  -> p&nz   "positive" = sign bit clear AND nonzero
 *   .testn  -> n&nz   "negative"
 *
 * If `POSITIVE` already meant "> 0", `.testp` would not need to add `nz`.
 * Hence the microkernel idiom for testing bit N:
 * `shl.testns p0, rX, #(31 - N)` shifts the bit into the sign position and
 * tests that it is clear.
 */
#define USE1_TEST_CRCOMB_AND        0x00000080U
#define USE1_TEST_PDST_SHIFT        2
#define USE1_TEST_PDST_MASK         0x3
#define USE1_TEST_CHANCC_SHIFT      4
#define USE1_TEST_CHANCC_MASK       0x7
#define USE1_TEST_CHANCC_SELECT0   0
#define USE0_TEST_WBEN               0x00100000U
#define USE0_TEST_ALUSEL_SHIFT      18
#define USE0_TEST_ALUSEL_MASK       0x3
#define USE0_TEST_ALUSEL_I16       1
#define USE0_TEST_ALUSEL_BITWISE    3
#define USE0_TEST_ALUOP_SHIFT       14
#define USE0_TEST_ALUOP_I16_ISUB   7
#define USE0_TEST_ALUOP_MASK        0xF
#define USE0_TEST_ALUOP_BW_AND      0
#define USE0_TEST_ALUOP_BW_OR       1
#define USE0_TEST_ALUOP_BW_XOR      2
#define USE0_TEST_ALUOP_BW_SHL      3
#define USE0_TEST_ALUOP_BW_SHR      4
#define USE0_TEST_ALUOP_BW_ASR      7

/*
 * Bitwise operations — `sgxdefs.h:5603..5630`. The opcode selects a GROUP
 * (ANDOR, XOR, SHLROL, SHRASR); one-bit OP2 (bit 3) selects within it.
 * SRC2INV inverts the second source; SRC2ROT rotates it before the operation.
 */
#define USE1_BITWISE_OP2_SHIFT      3
#define USE1_BITWISE_OP2_MASK       0x1
#define USE1_BITWISE_SRC2INV        0x00000800U
#define USE1_BITWISE_SRC2ROT_SHIFT  6
#define USE1_BITWISE_SRC2ROT_MASK   0x1F
#define USE1_BITWISE_PARTIAL        0x00000004U

/*
 * ⚠ The bitwise operation immediate is NOT the 7-bit `SRC2`; it is 16 bits,
 * assembled from three fields (`sgxdefs.h:5611..5636`):
 *
 *   bits  6:0  — `SRC2`               (word0 6:0)
 *   bits 13:7  — `SRC2IEXTLPSEL`      (word0 20:14)
 *   bits 15:14 — `SRC2IEXTH`          (word1 5:4)
 *
 * Hence `EURASIA_USE_BITWISE_MAXIMUM_UNROTATED_IMMEDIATE = 0xFFFF`.
 * Reading only `SRC2` made the model see masks as zero, so the event-check
 * chain never matched anything.
 */
#define USE0_BITWISE_SRC2IEXTLPSEL_SHIFT 14
#define USE0_BITWISE_SRC2IEXTLPSEL_MASK  0x7F
#define USE1_BITWISE_SRC2IEXTH_SHIFT     4
#define USE1_BITWISE_SRC2IEXTH_MASK      0x3

/*
 * MOVC — `sgxdefs.h:5375..5393`. TSTDTYPE = UNCOND means a normal,
 * unconditional `mov dst, src1`; the microkernel uses this form to copy
 * secondary attributes into temporary registers.
 */
#define USE1_MOVC_TSTDTYPE_SHIFT    8
#define USE1_MOVC_TSTDTYPE_MASK     0x7
#define USE1_MOVC_TSTDTYPE_UNCOND   0

/* EMIT — `sgxdefs.h:6684..6702`. */
#define USE1_EMIT_TARGET_SHIFT      14
#define USE1_EMIT_TARGET_MASK       0x3
#define USE1_EMIT_TARGET_PDS        2

/*
 * PDS program sideband 0 — `sgxdefs.h:7298..7306`. The model gets the
 * running program's data-segment size from this field, not an assumption:
 * PDSDATASIZE (bits 27:23) is in 16-byte units. In our image,
 * `mov r0, #0x01800800` (label `IUKP1_PDSConstSizeSec`) yields
 * (0x01800800 >> 23) & 0x1F = 3, or 48 bytes: exactly 3 parser rows.
 */
#define PDSSB0_PDSDATASIZE_SHIFT    23
#define PDSSB0_PDSDATASIZE_MASK     0x1F
#define PDSSB0_USEATTRSIZE_SHIFT    8
#define PDSSB0_USEATTRSIZE_MASK     0xFF
#define PDSSB0_SIZE_ALIGNSHIFT      4

/* Number of USE temporary registers held by the microkernel (sgx_mkif.h:165). */
#define USE_NUM_TEMPS               32
#define USE_NUM_PREDICATES          4

#endif /* HW_DISPLAY_CLARION_SGX_H */
