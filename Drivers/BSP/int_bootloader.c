#include "int_bootloader.h"
#include <string.h>

static uint8_t uart_rec_buff[PACKET_DATA_SIZE] = {0};

volatile uint8_t uart_rx_ready = 0U;
volatile uint32_t uart_total_len = 0U;

void bootloader_init(void)
{
    uart_rx_ready = 0U;
    uart_total_len = 0U;

    BootCache_Init();

    __HAL_UART_CLEAR_IDLEFLAG(&huart1);
    __HAL_UART_CLEAR_OREFLAG(&huart1);

    HAL_UARTEx_ReceiveToIdle_IT(&huart1,
                               uart_rec_buff,
                               PACKET_DATA_SIZE);
}

void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart,
                               uint16_t Size)
{
    HAL_StatusTypeDef status;

    if ((huart == NULL) || (huart->Instance != USART1))
    {
        return;
    }

    status = Boot_WriteCache(uart_rec_buff, Size);

    /*
     * 当前暂未定义可靠的文件结束标志，
     * 因此这里先不根据Size调用Boot_FlushCache。
     */
    /* if ((status == HAL_OK) && (Size < PACKET_DATA_SIZE))
    {
        status = Boot_FlushCache();
    } */

    if (status == HAL_OK)
    {
        uart_total_len += Size;
        uart_rx_ready = 1U;
    }
    else
    {
        uart_rx_ready = 2U;
    }

    memset(uart_rec_buff, 0, sizeof(uart_rec_buff));

    HAL_UARTEx_ReceiveToIdle_IT(&huart1,
                               uart_rec_buff,
                               PACKET_DATA_SIZE);
}
