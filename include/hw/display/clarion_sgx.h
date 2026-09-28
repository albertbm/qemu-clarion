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
#define SGX_CR_CORE_ID              0x0020
#define SGX_CR_CORE_REVISION        0x0024

/*
 * --- Ідентичність ядра ------------------------------------------------
 *
 * `EUR_CR_CORE_REVISION` (`sgx540defs.h:249..261`) — це
 * `DESIGNER<<24 | MAJOR<<16 | MINOR<<8 | MAINTENANCE`.
 *
 * ЗВІДКИ ВЗЯТО ЗНАЧЕННЯ. Не з припущення і не з підгонки: **його називає сама
 * прошивка**. Обробник `HostKickGetMiscInfo` кладе у свій буфер власну
 * константу `ui32CoreRevSW`, зібрану трьома інструкціями
 * (`eventhandler.use.asm:1138..1141`), а заголовок DDK прямо каже, що ця
 * константа — «SW core revision converted to internal format (as used in the
 * **EUR_CR_CORE_REVISION** register)» (`sgxdefs.h:145..148`).
 *
 * У нашому образі ці три безпосередні операнди читаються дослівно
 * (GPU VA 0x0E400A40..0x0E400A88):
 *
 *   mov r1, #0x00010000   MAJ = 1
 *   or  r1, r1, #0x00000200   MIN = 2
 *   or  r1, r1, #0x00000000   MAINT = 0      ⇒ 0x00010200, тобто SGX540 r1.2.0
 *
 * А драйвер вимагає `ui32CoreRev == ui32CoreRevSW`, інакше
 * `PVRSRV_ERROR_BUILD_MISMATCH` (`srvkm/.../sgxinit.c:2085`); виняток у
 * таблиці рівно один — пара (0x10100, 0x10101), і до 0x10200 він не
 * стосується. Оскільки ця прошивка на справжньому HU працювала, справжній
 * кремній мусив повертати саме `0x00010200`.
 */
#define SGX_CORE_REVISION_QY8       0x00010200U

/*
 * ⚠ `EUR_CR_CORE_ID` — ПІДСТАВИ НЕМАЄ, і це сказано вголос.
 *
 * Мікроядро лише копіює цей регістр у `ui32CoreID`, а той в усьому DDK
 * потрапляє тільки в debug-друк (`sgxinit.c:1037`) і в значення, яке
 * `PVRSRVGetSGXRevDataKM` віддає тому, хто спитав (`sgxutils.c:1609`).
 * Жодна перевірка його не читає.
 *
 * Тому тут нуль — і це інший випадок, ніж `MNE_CR_EVENT_STATUS`, де нуль
 * міняв потік керування й крутив мікроядро вічно. Значення, яке контракт не
 * читає, подати можна; значення, від якого залежить поведінка, — ні.
 * Модель друкує цей вибір при кожному читанні. Перевизначити:
 * `QY8_SGX_CORE_ID=` (і `QY8_SGX_CORE_REV=` для ревізії).
 */
#define SGX_CORE_ID_UNKNOWN         0x00000000U
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

/* SGX540 DDK sgxfeaturedefs.h: SGX_FEATURE_USE_NUMBER_PC_BITS = 12. */
#define SGX_FEATURE_USE_NUMBER_PC_BITS 12

/*
 * MNE — вузол системного кешу (`eurasia/hwdefs/mnemedefs.h`). Мікроядро
 * проходить через нього щоразу, коли хост просить `SGXMKIF_CC_INVAL_BIF_SL`.
 *
 * ⚠ На ЦЬОМУ ядрі запит «інвалідувати все» стоїть у біті 0 самого
 * `MNE_CR_CTRL`, а не в окремому `MNE_CR_CTRL_INVAL` (0x0D20): у
 * `usedefs.h` це гілка `#if !defined(MNE_CR_CTRL_INVAL)`, і прошивка
 * підтвердила саме її — `ldr 0xD00` -> `or #1` -> `str 0xD00` -> `ldr 0xD14`,
 * інструкція в інструкцію. Тому `MNE_CR_CTRL_INVAL_ALL` тут описує біт у
 * 0x0D00, хоча ім'я маски в заголовку належить регістру 0x0D20.
 *
 * Протокол рівневий: запит виставляють, чекають на `EVENT_STATUS.INVAL`,
 * гасять його через `EVENT_CLEAR`, і лише потім знімають сам запит.
 */
#define SGX_CR_MNE_CTRL             0x0D00
#define SGX_CR_MNE_CTRL_INVAL_ALL   0x00000001U
#define SGX_CR_MNE_EVENT_STATUS     0x0D14
#define SGX_CR_MNE_EVENT_STATUS_INVAL 0x00000001U
#define SGX_CR_MNE_EVENT_CLEAR      0x0D18
#define SGX_CR_MNE_EVENT_CLEAR_INVAL  0x00000001U

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
#define USE1_OP_IMAE                21
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

/*
 * Короткий предикат (біти 26:25) — `sgxdefs.h:5184..5190`. Цілочисельні
 * інструкції (IMAE і решта групи INT) несуть предикат ТУТ, а не в полі
 * EPRED: у них біт 27 зайнято номером опкоду, а 26:25 лишилися предикату.
 * Сплутати простір легко, тому поля названо окремо.
 */
#define USE1_SPRED_SHIFT            25
#define USE1_SPRED_MASK             0x3
#define USE1_SPRED_ALWAYS           0
#define USE1_SPRED_P0               1
#define USE1_SPRED_P1               2
#define USE1_SPRED_NOTP0            3

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

/*
 * Індексована адресація. Приймач `mov` у банку `D1EXTBANK_INDEX`
 * (`sgxdefs.h:5260`) несе в полі номера не номер регістра, а МАСКУ: еталонний
 * асемблер приймає лише 1, 2, 3 і відкидає решту з «Invalid index mask»
 * (`useasm.c:1920..1927`). Індексований операнд (`S12EXTBANK_INDEXED` як
 * джерело, `D1STDBANK_INDEXED` як приймач) тримає в полі номера трійку
 * {банк, вибір регістра, зсув}: біти 6:5 — банк, біт 4 — IDXSEL, біти 3:0 —
 * зсув (`sgxdefs.h:7719..7733`, складання — `useasm.c:1023..1060`).
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

/*
 * `SETL` і `SAVL` — це `mov pclink, src1` і `mov dst, pclink`
 * (`useasm.c:14077..14120`): окремої інструкції «запиши регістр зв'язку» в
 * асемблері немає, є спеціальна форма `mov`. Джерело SETL читається як SRC1,
 * приймач SAVL — звичайним полем призначення.
 *
 * ⚠ Регістр зв'язку тримає НОМЕР ІНСТРУКЦІЇ відносно вікна коду, а не адресу:
 * host-kick обробники мікроядро віддає хостові саме так —
 * `ui32ServiceAddress = <зсув мітки> / EURASIA_USE_INSTRUCTION_SIZE`
 * (`srvinit/devices/sgx/sgxinit.c:690..692`), і це значення потрапляє в
 * `pclink` без жодного перетворення. Той самий простір, що й у цілі `ba`.
 */

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
 * Предикат LIMM живе у власному полі (біти 11:9, `sgxdefs.h:7443..7444`), а
 * набір значень у нього той самий, що й у звичайного EPRED: еталонний
 * асемблер кладе туда результат того самого `EncodePredicate(..., FALSE)`
 * (`useasm.c:14012`, `useasm.c:2664..2671`).
 */
#define USE1_LIMM_EPRED_SHIFT       9
#define USE1_LIMM_EPRED_MASK        0x7

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
#define USE1_LDST_IMODE_PRE         1
#define USE1_LDST_IMODE_POST        2
#define USE1_LDST_IMODE_RESERVED    3

/*
 * Банк приймача LD — окремий однобітовий прапорець, а не поле D1BANK
 * (`sgxdefs.h:6397..6399`). Асемблер прямо забороняє будь-який інший банк:
 * «The destination for an LD must be the primary attribute bank or the
 * temporary bank» (`useasm.c:12266`).
 */
#define USE1_LDST_DBANK_PRIMATTR    0x00000080U

/*
 * ⚠ `MOEEXPAND` — ІНВЕРСНИЙ прапорець режиму вибірки (`sgxdefs.h:6376`).
 * Еталонний декодер: `if (!(uInst1 & MOEEXPAND)) FETCHENABLE`
 * (`usedisasm.c:2286..2288`). Тобто:
 *
 *   MOEEXPAND = 1 → звичайний доступ, повтори розгортає MOE;
 *   MOEEXPAND = 0 → режим ВИБІРКИ (`.fetchN`): N поспіль двослів у N поспіль
 *                   регістрів приймача, MOE в цьому не бере участі.
 *
 * Лічильник в обох режимах той самий — `RMSKCNT`, і його значення на одиницю
 * менше за N (`useasm.c:12176`: `(uRptCount - 1) << RMSKCNT_SHIFT`;
 * `usedisasm.c:2306`: `uMaskCount + 1`).
 */
#define USE1_LDST_MOEEXPAND         0x00200000U
#define USE1_RMSKCNT_SHIFT          12
#define USE1_RMSKCNT_MASK           0xF

/*
 * `.fcfill` = force cache line fill (`sgxdefs.h:6413`) — підказка кешу даних.
 * Кеша в моделі немає, тому вимога виконується тривіально, а не «пропущена».
 */
#define USE1_LDST_FCLFILL           0x00000002U
#define USE1_LDST_RANGEENABLE       0x00000040U
#define USE1_LDST_INCSGN            0x00000008U

/*
 * IMAE — цілочисельне множення з додаванням, `sgxdefs.h:5155, 6021..6055`.
 * Семантика — з коментаря самого компілятора IMG (`usc2/finalise.c:3137`):
 *
 *   IMAE  DST, SRC0, #SRC1, SRC2   //  DST = SRC0 * SRC1 + SRC2
 *
 * Множники — ПІВСЛОВА (16 біт), і яке саме півслово, обирають прапорці
 * SRC0H/SRC1H; додаток SRC2 має власний тип (16 із нулями, 16 зі знаком або
 * повні 32 біти). Псевдоінструкція `iaddu32 d, a, b` — це той самий IMAE з
 * SRC1 = #1 (`useasm.c:6217..6252`), тому окремо її реалізовувати не треба.
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

/* Лічильник повторів цілочисельної групи — `sgxdefs.h:5641..5643`. */
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
 * ⚠ У `TEST` ДВІ незалежні перевірки — нуля й знака, — і як їх поєднати,
 * каже окремий біт: `CRCOMB` виставлений = AND, знятий = OR
 * (`sgxdefs.h:5509`, `usedisasm.c:9385`). Значення `NONE` в обох полях —
 * це не «немає перевірки», а «перевірка завжди істинна»
 * (`usedisasm.c:9222..9232`: `STST_NONE -> SIGN_TRUE`,
 * `ZTST_NONE -> ZERO_TRUE`), тому з AND вона просто не заважає.
 *
 * `POSITIVE` означає «біт знака ЗНЯТО», а не «> 0», і це видно з самого
 * асемблера (`use.l:761..767`), де суфікси розкриваються дослівно:
 *
 *   .tests  -> n&t    знак стоїть
 *   .testns -> p&t    знак знято          <- наш випадок
 *   .testp  -> p&nz   «додатне» = знак знято І не нуль
 *   .testn  -> n&nz   «від'ємне»
 *
 * Тобто якби `POSITIVE` уже означало «> 0», то `.testp` не мусив би окремо
 * додавати `nz`. Звідси ідіом мікроядра «перевірити біт N»:
 * `shl.testns p0, rX, #(31 - N)` — зсунути біт у знак і перевірити, що він
 * знятий.
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
