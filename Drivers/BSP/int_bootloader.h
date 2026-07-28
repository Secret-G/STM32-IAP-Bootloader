#ifndef __INT_BOOTLOADER_H
#define __INT_BOOTLOADER_H

#include "usart.h"
#include "boot_cache.h"

#define PACKET_DATA_SIZE 256U


void bootloader_init(void);

void Bootloader_Process(void);

void Boot_JumpToApp(void);
HAL_StatusTypeDef Boot_CopyToRun(uint32_t source_addr, uint32_t image_size);

#endif
