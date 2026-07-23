#ifndef __BOOT_FLASH_H
#define __BOOT_FLASH_H

#include "stm32f4xx_hal.h"

/* Flash基地址 */
#define FLASH_BASE_ADDR          0x08000000U

/*Flash结束地址*/
#define FLASH_END_ADDR           (FLASH_BASE_ADDR + 0x100000U -1U)

/* Sector起始地址 */
#define FLASH_SECTOR_0_ADDR      (FLASH_BASE_ADDR + 0x0000U)
#define FLASH_SECTOR_1_ADDR      (FLASH_BASE_ADDR + 0x4000U)
#define FLASH_SECTOR_2_ADDR      (FLASH_BASE_ADDR + 0x8000U)
#define FLASH_SECTOR_3_ADDR      (FLASH_BASE_ADDR + 0xC000U)
#define FLASH_SECTOR_4_ADDR      (FLASH_BASE_ADDR + 0x10000U)
#define FLASH_SECTOR_5_ADDR      (FLASH_BASE_ADDR + 0x20000U)
#define FLASH_SECTOR_6_ADDR      (FLASH_BASE_ADDR + 0x40000U)
#define FLASH_SECTOR_7_ADDR      (FLASH_BASE_ADDR + 0x60000U)
#define FLASH_SECTOR_8_ADDR      (FLASH_BASE_ADDR + 0x80000U)
#define FLASH_SECTOR_9_ADDR      (FLASH_BASE_ADDR + 0xA0000U)
#define FLASH_SECTOR_10_ADDR     (FLASH_BASE_ADDR + 0xC0000U)
#define FLASH_SECTOR_11_ADDR     (FLASH_BASE_ADDR + 0xE0000U)


/* APP */
#define APP_START_ADDR           FLASH_SECTOR_4_ADDR
#define APP_END_ADDR             FLASH_END_ADDR

/* Bootloader */
#define BOOTLOADER_START_ADDR    FLASH_SECTOR_0_ADDR
#define BOOTLOADER_END_ADDR      (APP_START_ADDR - 1U)


#define INVALID_SECTOR 0xFFFFFFFFU

/* Sector数量 */
#define FLASH_SECTOR_NUM         12


uint32_t Flash_GetSector(uint32_t addr);

HAL_StatusTypeDef Flash_EraseSector(uint32_t sector);

HAL_StatusTypeDef Flash_EraseApp(void);

HAL_StatusTypeDef Flash_Write(uint32_t addr,uint8_t *data,uint32_t len);





#endif
