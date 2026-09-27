/*
 * PowerVR SGX плати Clarion QY8XXX — пасивна модель MMIO і трасувальник BIF/MMU.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_DISPLAY_CLARION_SGX_H
#define HW_DISPLAY_CLARION_SGX_H

#define TYPE_CLARION_SGX "clarion-sgx"

/*
 * База — з живого прогону, не з аналогії. `ddi_ncg.dll` мапить блок
 * MmMapIoSpace і тримає ядерний вказівник у `psDevInfo->pvRegsBaseKM`; зонд
 * tools/qy8_sgx_recon_probe.py репозиторію nissan-can-explore знімає його на
 * точці зупинки в `SGXInitialise` і бачить фізичну базу 0xFCE00000.
 *
 * Розмір 0x4000 — стандартний банк регістрів SGX у DDK. Найбільший зсув, який
 * ми справді бачили від гостя, — 0x0C84 (`EUR_CR_BIF_DIR_LIST_BASE0`), тож
 * вікна досить із запасом; воно ні з чим відомим не перетинається.
 */
#define CLARION_SGX_BASE        0xFCE00000
#define CLARION_SGX_SIZE        0x4000

/*
 * Зсуви регістрів — із публічних заголовків SGX (`sgx531defs.h` і
 * однакові в 530/535/540/545 там, де ми ними користуємось). Модель НЕ
 * тлумачить їхніх полів, окрім масок адрес, які в цих же заголовках і задані.
 */
#define SGX_CR_CORE_ID              0x0020
#define SGX_CR_CORE_REVISION        0x0024
#define SGX_CR_SOFT_RESET           0x0080
#define SGX_CR_EVENT_HOST_ENABLE2   0x0110
#define SGX_CR_EVENT_HOST_ENABLE    0x0130
#define SGX_CR_USE_CODE_BASE(x)     (0x0A0C + 4 * (x))
#define SGX_CR_PDS_EXEC_BASE        0x0AB8
#define SGX_CR_EVENT_KICK2          0x0AC0
#define SGX_CR_EVENT_KICKER         0x0AC4
#define SGX_CR_EVENT_KICK           0x0AC8
#define SGX_CR_EVENT_TIMER          0x0ACC
#define SGX_CR_BIF_CTRL             0x0C00
#define SGX_CR_BIF_DIR_LIST_BASE0   0x0C84

/*
 * Триплет task-control події «Other», який init-script прошивки пише ОСТАННІМ
 * перед kick'ом.
 *
 * ⚠ Раніше тут стояло «імені в жодному з публічних заголовків немає» і
 * робоча назва `SGX_CR_QY8_TASK_*`. Це було неправдою: імена є в
 * `eurasia/hwdefs/sgx540defs.h:2135..2157` того самого дерева DDK — просто
 * ми доти дивилися в заголовки інших ядер. Знайдено під час T6
 * (docs/sgx/23), підтверджено дослівним збігом зсувів.
 *
 *   0x0A68 EUR_CR_EVENT_OTHER_PDS_EXEC  ADDR_MASK 0x03FFFFF0 — адреса
 *          програми PDS відносно EUR_CR_PDS_EXEC_BASE (наш прогін: 0x0080C180)
 *   0x0A6C EUR_CR_EVENT_OTHER_PDS_DATA  SIZE_MASK 0x3F — розмір сегмента
 *          даних у одиницях по 16 Б (наш прогін: 2 -> 32 Б, і це точно
 *          збігається з «2 рядки × 2 банки × 2 дв.сл.»)
 *   0x0A70 EUR_CR_EVENT_OTHER_PDS_INFO  DM / ATTRIBUTE_SIZE / USESECEXEC
 *
 * Той самий триплет код мікроядра пише з боку USE інструкціями
 * `str #666/#667` (номер = байтовий зсув / 4) — docs/sgx/23 §3.
 * Докази ролі й ланцюга — docs/sgx/09-edm-boot-locator.md.
 */
#define SGX_CR_EVENT_OTHER_PDS_EXEC 0x0A68
#define SGX_CR_EVENT_OTHER_PDS_DATA 0x0A6C
#define SGX_CR_EVENT_OTHER_PDS_INFO 0x0A70
#define SGX_CR_QY8_TASK_W3          0x0A74

#define SGX_EVENT_OTHER_PDS_EXEC_ADDR_MASK  0x03FFFFF0U
#define SGX_EVENT_OTHER_PDS_DATA_SIZE_MASK  0x0000003FU

/* Маски полів адрес — дослівно з sgx*defs.h. */
#define SGX_PDS_EXEC_BASE_ADDR_MASK     0x0FF00000U
#define SGX_EVENT_KICKER_ADDR_MASK      0x0FFFFFF0U
#define SGX_USE_CODE_BASE_ADDR_MASK     0x00FFFFFFU
#define SGX_USE_CODE_BASE_DM_MASK       0x03000000U
#define SGX_USE_CODE_BASE_DM_SHIFT      24
#define SGX_BIF_DIR_LIST_BASE_ADDR_MASK 0xFFFFF000U

#define SGX_CR_EVENT_KICK2_NOW          0x00000001U

/*
 * Формат MMU SGX — DDK 1.7 services4/srvkm/hwdefs/sgxmmu.h, варіант без
 * SGX_FEATURE_36BIT_MMU: device-VA = 10 біт PDE + 10 біт PTE + 12 біт зсуву,
 * по 1024 записи в каталозі й у таблиці, біт 0 = «валідний».
 */
#define SGX_MMU_PAGE_SHIFT      12
#define SGX_MMU_PAGE_SIZE       (1u << SGX_MMU_PAGE_SHIFT)
#define SGX_MMU_PD_SHIFT        22
#define SGX_MMU_ENTRIES         1024
#define SGX_MMU_ENTRY_VALID     0x00000001U
#define SGX_MMU_ENTRY_ADDR_MASK 0xFFFFF000U

/*
 * --- Кодування PDS (SGX540) -------------------------------------------
 *
 * Усе нижче — дослівно з публічного DDK, гілка SGX540 (без
 * `SGX_FEATURE_USE_UNLIMITED_PHASES` і без `SGX_FEATURE_PDS_EXTENDED_SOURCES`):
 * `eurasia/hwdefs/sgxdefs.h` і `eurasia/codegen/pds/pds.c` дерева
 * GFX_Linux_DDK @ f184ac914561fa100a5c92a488df777de8785f93. Нічого не вгадано;
 * розбір і посилання на рядки — docs/sgx/13 і docs/sgx/14.
 */
#define PDS_INSTRUCTION_SIZE        4
#define PDS_INST_SHIFT              30      /* біти 31:30 — група */
#define PDS_INST_MOV                0
#define PDS_INST_ARITH              1
#define PDS_INST_FLOW               2
#define PDS_INST_LOGIC              3
#define PDS_TYPE_SHIFT              27      /* біти 29:27 — тип */
#define PDS_TYPE_MOVS               0
#define PDS_TYPE_MOV16              1
#define PDS_TYPE_MOV32              2
#define PDS_TYPE_TSTZ               0       /* у групі FLOW */
#define PDS_TYPE_BRA                2
#define PDS_TYPE_HALT               5
#define PDS_TYPE_TSTN               1       /* у групі FLOW */
#define PDS_TYPE_CALL               3
#define PDS_TYPE_RTN                4
#define PDS_TYPE_NOP                6
#define PDS_CC_SHIFT                24      /* біти 26:24 */
#define PDS_CC_P0                   0
#define PDS_CC_P1                   1
#define PDS_CC_P2                   2
#define PDS_CC_IF0                  3
#define PDS_CC_IF1                  4
#define PDS_CC_ALUZ                 5
#define PDS_CC_ALUN                 6
#define PDS_CC_ALWAYS               7

/*
 * TSTZ/TSTN — `sgxdefs.h:3068..3090`. Предикат p0..p2 (біти 2:0) дістає
 * результат порівняння з нулем одного з двох джерел; SRCSEL (біт 8) вибирає,
 * якого саме. SRC1 — ds0[] або вхідний регістр ir0/ir1, SRC2 — завжди ds1[].
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
 * BRA/CALL — `sgxdefs.h:3106..3109` (`FLOW_DEST` біти 18:0).
 *
 * ⚠ Тут легко помилитися на один зсув, і ми на цьому вже спіймалися.
 * `pdsasm/main.c:4736` рахує `uDest = uLabelOffset << ALIGNSHIFT`, але
 * `PDSEncodeBRA` (`sgxpdsdefs.h:775..782`) кладе в поле `uDest >> ALIGNSHIFT`.
 * Зсуви взаємно скорочуються, отже в полі лежить **номер інструкції**, а
 * байтовий зсув від початку сегмента коду = поле << ALIGNSHIFT.
 */
#define PDS_FLOW_DEST_MASK          0x7FFFF
#define PDS_FLOW_DEST_ALIGNSHIFT    2

/*
 * MOVS, гілка БЕЗ `SGX_FEATURE_PDS_EXTENDED_SOURCES` (тобто наша, SGX540).
 * ⚠ Ця гілка інша, ніж розширена: `SRC2SEL` там немає взагалі (джерело 2 —
 * завжди банк DS1), `SRC2` стоїть на 13, а свізли — на 11/9/7/5, не 10/8/6/4.
 * `SRC1`/`SRC2` індексують ЧЕТВЕРНІ слова, тож індекс двійного слова —
 * `src * PDS_NUM_DWORDS_PER_QWORD + (swiz & 1)` (`pdsdisasm.c`).
 */
#define PDS_MOVS_SRC1SEL_SHIFT      23
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

/* Розмір банку datastore, `sgxdefs.h:2485..2491`: 0..47 константи, 48..63 temp. */
#define PDS_DATASTORE_PERBANKSIZE   64

/* Розкладка сегмента даних: PDS_NUM_DWORDS_PER_ROW = 2 для SGX540. */
#define PDS_NUM_DWORDS_PER_ROW      2
#define PDS_NUM_USE_TASK_CONTROL_WORDS  3
#define PDS_NUM_DMA_CONTROL_WORDS   2

/* DOUTD — DMA-інтерфейс PDS */
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
 * Вторинні атрибути мікроядра. Розкладку задає `PVRSRV_SGX_EDMPROG_SECATTR`
 * (`services4/srvinit/devices/sgx/sgx_mkif.h:144..156`): двійне слово 0 —
 * `sTA3DCtl`, 1 — `sHostCtl`, 2 — `sCCBCtl`, далі `sMKState`. А
 * `usedefs.h:68..72` дає імена, якими користується сам код USE:
 * `R_HostCtl = SA(sHostCtl)`, тобто рівно `sa[1]` у лістингу мікроядра.
 *
 * `SGX_UKERNEL_SA_BURST_SIZE = 16` двійних слів (`sgx_mkif.h:171`), а
 * загальний розмір — `SGX_UKERNEL_NUM_SEC_ATTRIB` (`sgx_mkif.h:174`); для
 * нашої збірки DOUTD везе 2 рядки по 16, тобто 32 двійних слова. Банк тримаємо
 * з запасом до максимуму поля AO.
 */
#define SGX_SA_DWORDS               (PDS_DOUTD1_AO_MASK + 1)
#define SGX_SA_TA3DCTL              0
#define SGX_SA_HOSTCTL              1
#define SGX_SA_CCBCTL               2

/* DOUTU — інтерфейс запуску задачі USE */
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

#endif /* HW_DISPLAY_CLARION_SGX_H */
