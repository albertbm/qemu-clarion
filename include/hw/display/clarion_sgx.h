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
#define SGX_CR_EVENT_PDS_ENABLE     0x0A58
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
 * Програмна подія. ⚠ Нумерація РІЗНА у двох просторах, і сплутати їх легко:
 *
 *   регістри   `EUR_CR_EVENT_{STATUS,HOST_ENABLE,HOST_CLEAR,PDS_ENABLE}`
 *              — `SW_EVENT` це біт **14** (`sgx540defs.h:536,631,726,2043`);
 *   вхід PDS   `ir1` — `EURASIA_PDS_IR1_EDM_EVENT_SWEVENT` це біт **8**
 *              (`sgxdefs.h:3566`, гілка НЕ-543/544/554, тобто наша).
 *
 * Відповідність між ними — за іменем тієї самої події, а не за позицією.
 */
#define SGX_EVENT_SW_EVENT_MASK         0x00004000U
#define PDS_IR1_EDM_EVENT_SWEVENT       (1u << 8)
#define PDS_IR1_EDM_EVENT_KICKPTR_SHIFT 24

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
 * Логічні операції — `sgxdefs.h:3119..3176`, гілка БЕЗ
 * `SGX_FEATURE_PDS_EXTENDED_SOURCES` (наша, SGX540): `SRC2SEL` немає взагалі,
 * друге джерело — завжди банк DS1, і `SRC2` стоїть на 10, а не на 9.
 */
#define PDS_TYPE_OR                 0       /* у групі LOGIC */
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
#define PDS_MOVS_SRC1SEL_REG        1
/*
 * Коли SRC1SEL = REG, поле SRC1 індексує не datastore, а вхідні регістри
 * задачі (`sgxdefs.h:2674..2676`).
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

/*
 * DOUTA — запис у ПЕРВИННІ атрибути задачі USE (`sgxdefs.h:3654..3662`).
 * Слово 0 — самі дані (усі 32 біти), слово 1 несе `AO` — зсув у банку
 * атрибутів, біти 18:8 у нашій гілці (CLRMSK `0xFFF800FF`).
 *
 * Саме цим обробник подій мікроядра починає роботу:
 * `movs douta, ir0, INPUT_IR0_PA_DEST` і те саме для `ir1`
 * (`services4/srvinit/devices/sgx/eventhandler.pds.asm:84..85`) — тобто
 * перекладає прапорці події з вхідних регістрів у атрибути, щоб код USE міг
 * перевірити, яка подія сталася.
 */
#define PDS_NUM_ATTRIB_CONTROL_WORDS 2
#define PDS_DOUTA1_AO_SHIFT         8
#define PDS_DOUTA1_AO_MASK          0x7FF

/* Банк первинних атрибутів; розмір — за максимумом поля AO. */
#define SGX_PA_DWORDS               (PDS_DOUTA1_AO_MASK + 1)

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

/*
 * --- Кодування USE (SGX540) -------------------------------------------
 *
 * Дослівно з `eurasia/hwdefs/sgxdefs.h` того самого дерева DDK. Інструкція —
 * пара 32-бітних слів little-endian: word0 (низ) і word1 (верх). Опкод — у
 * word1, біти 31:27.
 *
 * ⚠ Біт 18 word1 КОНТЕКСТНО-ЗАЛЕЖНИЙ: для інструкцій без операнда S0 це
 * `END` (кінець задачі), для LD/ST це `S0BEXT`, для TEST — `PARTIAL`.
 * Не можна перевіряти його наосліп для всіх опкодів.
 */
#define USE_INST_SIZE               8

#define USE1_OP_SHIFT               27
#define USE1_OP_MOVC                5
#define USE1_OP_TEST                9
#define USE1_OP_ANDOR               10
#define USE1_OP_XOR                 11
#define USE1_OP_SHLROL              12
#define USE1_OP_SHRASR              13
#define USE1_OP_LD                  29
#define USE1_OP_ST                  30
#define USE1_OP_SPECIAL             31

/* Розширений предикат (біти 26:24) — `sgxdefs.h:5166..5176`. */
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

/* Прапорці word1 — `sgxdefs.h:5198..5206`. */
#define USE1_END                    0x00040000U
#define USE1_S0BEXT                 0x00040000U
#define USE1_S1BEXT                 0x00020000U
#define USE1_S2BEXT                 0x00010000U
#define USE1_DBEXT                  0x00080000U

/* Поля регістрів — `sgxdefs.h:5225..5305`. */
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

/* Банки. Std — коли відповідний *BEXT знято, Ext — коли виставлено. */
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

/* SPECIAL: категорія (біти 21:20) — `sgxdefs.h:6453..6459`. */
#define USE1_SPECIAL_OPCAT_SHIFT    20
#define USE1_SPECIAL_OPCAT_MASK     0x3
#define USE1_SPECIAL_OPCAT_FLOWCTRL 0
#define USE1_SPECIAL_OPCAT_MOECTRL  1
#define USE1_SPECIAL_OPCAT_OTHER    2
#define USE1_SPECIAL_OPCAT_VISTEST  3

/* FLOWCTRL: підопкод (біти 8:6) — `sgxdefs.h:6496..6507`. */
#define USE1_FLOWCTRL_OP2_SHIFT     6
#define USE1_FLOWCTRL_OP2_MASK      0x7
#define USE1_FLOWCTRL_OP2_BA        0
#define USE1_FLOWCTRL_OP2_BR        1
#define USE1_FLOWCTRL_OP2_LAPC      2
#define USE1_FLOWCTRL_OP2_SETL      3
#define USE1_FLOWCTRL_OP2_SAVL      4
#define USE1_FLOWCTRL_OP2_NOP       5

/* Гілка: `sgxdefs.h:6518..6521`. Зсув у word0 — НОМЕР ПАРИ, крок 8 Б. */
#define USE1_BRANCH_SAVELINK        0x00000200U
#define USE0_BRANCH_OFFSET_MASK     0x000FFFFFU

/* OTHER: підопкод (біти 26:24) — `sgxdefs.h:6656..6666`. */
#define USE1_OTHER_OP2_SHIFT        24
#define USE1_OTHER_OP2_MASK         0x7
#define USE1_OTHER_OP2_IDF          0
#define USE1_OTHER_OP2_WDF          1
#define USE1_OTHER_OP2_EMIT         3
#define USE1_OTHER_OP2_LIMM         4
#define USE1_OTHER_OP2_LOCKRELEASE  5
#define USE1_OTHER_OP2_LDRSTR       6
#define USE1_OTHER_OP2_WOP          7

/* MOECTRL: підопкод (біти 26:24) — `sgxdefs.h:6553..6560`. */
#define USE1_MOECTRL_OP2_SHIFT      24
#define USE1_MOECTRL_OP2_MASK       0x7
#define USE1_MOECTRL_OP2_SMLSI      2

/*
 * LIMM — `sgxdefs.h:7435..7447`. 32-бітна константа розрізана на три шматки:
 * біти 20:0 у word0, біти 25:21 у word1[8:4], біти 31:26 у word1[17:12].
 */
#define USE0_LIMM_IMML21_MASK       0x001FFFFFU
#define USE1_LIMM_IMM2521_SHIFT     4
#define USE1_LIMM_IMM2521_MASK      0x1F
#define USE1_LIMM_IMM3126_SHIFT     12
#define USE1_LIMM_IMM3126_MASK      0x3F

/*
 * LDRSTR (`str`/`ldr`) — `sgxdefs.h:7461..7475`. Номер спецрегістра склеєний
 * із двох полів: SRC2 (біти 6:0) і SRC2EXT (біти 20:14), зсунутого на 7.
 * За T6 (docs/sgx/23) номер × 4 = байтовий зсув регістра SGX.
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

/* TEST — `sgxdefs.h:5488..5587`. */
#define USE1_TEST_ZTST_SHIFT        8
#define USE1_TEST_ZTST_MASK         0x3
#define USE1_TEST_ZTST_NONE         0
#define USE1_TEST_ZTST_ZERO         1
#define USE1_TEST_ZTST_NOTZERO      2
#define USE1_TEST_STST_SHIFT        10
#define USE1_TEST_STST_MASK         0x3
#define USE1_TEST_STST_NONE         0
#define USE1_TEST_PDST_SHIFT        2
#define USE1_TEST_PDST_MASK         0x3
#define USE0_TEST_WBEN               0x00100000U
#define USE0_TEST_ALUSEL_SHIFT      18
#define USE0_TEST_ALUSEL_MASK       0x3
#define USE0_TEST_ALUSEL_BITWISE    3
#define USE0_TEST_ALUOP_SHIFT       14
#define USE0_TEST_ALUOP_MASK        0xF
#define USE0_TEST_ALUOP_BW_AND      0
#define USE0_TEST_ALUOP_BW_OR       1
#define USE0_TEST_ALUOP_BW_XOR      2
#define USE0_TEST_ALUOP_BW_SHL      3
#define USE0_TEST_ALUOP_BW_SHR      4
#define USE0_TEST_ALUOP_BW_ASR      7

/*
 * Бітові операції — `sgxdefs.h:5603..5630`. Опкод задає ГРУПУ (ANDOR, XOR,
 * SHLROL, SHRASR), а однобітове поле OP2 (біт 3) вибирає всередині неї.
 * SRC2INV інвертує друге джерело, SRC2ROT крутить його перед операцією.
 */
#define USE1_BITWISE_OP2_SHIFT      3
#define USE1_BITWISE_OP2_MASK       0x1
#define USE1_BITWISE_SRC2INV        0x00000800U
#define USE1_BITWISE_SRC2ROT_SHIFT  6
#define USE1_BITWISE_SRC2ROT_MASK   0x1F
#define USE1_BITWISE_PARTIAL        0x00000004U

/*
 * ⚠ Безпосередній операнд бітових операцій — НЕ 7-бітний `SRC2`, а 16-бітний,
 * склеєний із трьох полів (`sgxdefs.h:5611..5636`):
 *
 *   біти  6:0  — `SRC2`               (word0 6:0)
 *   біти 13:7  — `SRC2IEXTLPSEL`      (word0 20:14)
 *   біти 15:14 — `SRC2IEXTH`          (word1 5:4)
 *
 * Звідси й `EURASIA_USE_BITWISE_MAXIMUM_UNROTATED_IMMEDIATE = 0xFFFF`.
 * Прочитавши лише `SRC2`, модель бачила маски як нулі — і ланцюг перевірок
 * подій провалювався наскрізь, нічого не збігаючи.
 */
#define USE0_BITWISE_SRC2IEXTLPSEL_SHIFT 14
#define USE0_BITWISE_SRC2IEXTLPSEL_MASK  0x7F
#define USE1_BITWISE_SRC2IEXTH_SHIFT     4
#define USE1_BITWISE_SRC2IEXTH_MASK      0x3

/*
 * MOVC — `sgxdefs.h:5375..5393`. Поле TSTDTYPE = UNCOND означає звичайний
 * `mov dst, src1` без умови; саме в такій формі мікроядро копіює вторинні
 * атрибути в тимчасові регістри.
 */
#define USE1_MOVC_TSTDTYPE_SHIFT    8
#define USE1_MOVC_TSTDTYPE_MASK     0x7
#define USE1_MOVC_TSTDTYPE_UNCOND   0

/* EMIT — `sgxdefs.h:6684..6702`. */
#define USE1_EMIT_TARGET_SHIFT      14
#define USE1_EMIT_TARGET_MASK       0x3
#define USE1_EMIT_TARGET_PDS        2

/*
 * Sideband 0 програми PDS — `sgxdefs.h:7298..7306`. Саме звідси модель бере
 * розмір сегмента даних запущеної програми, а не з припущення: поле
 * PDSDATASIZE (біти 27:23) рахується в одиницях по 16 Б. У нашому образі
 * `mov r0, #0x01800800` (мітка `IUKP1_PDSConstSizeSec`) дає
 * (0x01800800 >> 23) & 0x1F = 3, тобто 48 Б — рівно 3 «рядки» розбирача.
 */
#define PDSSB0_PDSDATASIZE_SHIFT    23
#define PDSSB0_PDSDATASIZE_MASK     0x1F
#define PDSSB0_USEATTRSIZE_SHIFT    8
#define PDSSB0_USEATTRSIZE_MASK     0xFF
#define PDSSB0_SIZE_ALIGNSHIFT      4

/* Кількість тимчасових регістрів USE, які тримає мікроядро (sgx_mkif.h:165). */
#define USE_NUM_TEMPS               32
#define USE_NUM_PREDICATES          4

#endif /* HW_DISPLAY_CLARION_SGX_H */
