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
 * Невідомий регістр, який init-script прошивки пише ОСТАННІМ перед kick'ом і
 * значення якого доведено є адресою: 0x0080C180, а
 * `EUR_CR_PDS_EXEC_BASE + 0x0080C180` точно дорівнює поверненому device-VA
 * однієї з алокацій SGX. Імені в жодному з семи публічних заголовків немає
 * (docs/sgx/03), тому називаємо його за роллю, яку довели, а не за здогадом:
 * носій адреси відносно PDS_EXEC_BASE. Докази — docs/sgx/09-edm-boot-locator.md.
 */
#define SGX_CR_QY8_TASK_ADDR        0x0A68
#define SGX_CR_QY8_TASK_W1          0x0A6C
#define SGX_CR_QY8_TASK_W2          0x0A70
#define SGX_CR_QY8_TASK_W3          0x0A74

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

#endif /* HW_DISPLAY_CLARION_SGX_H */
