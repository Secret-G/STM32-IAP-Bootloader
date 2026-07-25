#ifndef __INT_BOOTLOADER_H
#define __INT_BOOTLOADER_H

#include "usart.h"
#include "boot_cache.h"

#define PACKET_DATA_SIZE 256U

typedef enum
{
    BOOT_WAIT_COMMAND = 0,
    BOOT_ERASING,
    BOOT_RECEIVE_A,
    BOOT_RECEIVE_B,
    BOOT_FINISHING,
    BOOT_RECEIVE_ERROR

} Boot_ReceiveStateTypeDef;

extern volatile uint8_t uart_rx_ready;
extern volatile uint32_t uart_total_len;

void bootloader_init(void);

void Bootloader_Process(void);

void Boot_JumpToApp(void);
HAL_StatusTypeDef Boot_CopyToRun(uint32_t source_addr, uint32_t image_size);

#endif