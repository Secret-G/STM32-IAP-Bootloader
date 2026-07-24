#ifndef __INT_BOOTLOADER_H
#define __INT_BOOTLOADER_H

#include "usart.h"
#include "boot_cache.h"

#define PACKET_DATA_SIZE 256U

/*
 * uart_rx_ready:
 * 0 = 没有新的处理结果
 * 1 = 本次数据写入成功
 * 2 = 擦除或写入失败
 */
extern volatile uint8_t uart_rx_ready;
extern volatile uint32_t uart_total_len;

void bootloader_init(void);


void boot_loder_jump_to_app(void);
#endif
