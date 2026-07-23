#ifndef __int__BOOTLOADER_H
#define __int__BOOTLOADER_H

#include "usart.h"
#include "string.h"

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
#define APP_START_ADDR           FLASH_SECTOR_5_ADDR
#define APP_END_ADDR             FLASH_END_ADDR

/* Bootloader */
#define BOOTLOADER_START_ADDR    FLASH_SECTOR_0_ADDR
#define BOOTLOADER_END_ADDR      (APP_START_ADDR - 1U)


#define INVALID_SECTOR 0xFFFFFFFFU

/* Sector数量 */
#define FLASH_SECTOR_NUM         12


extern uint8_t sector_erase_flag[FLASH_SECTOR_NUM];

extern volatile uint8_t  uart_rx_ready;
extern volatile uint32_t uart_total_len;

#define PACKET_DATA_SIZE 256U

void bootloader_init(void);

uint32_t Flash_GetSector(uint32_t addr);


#endif
