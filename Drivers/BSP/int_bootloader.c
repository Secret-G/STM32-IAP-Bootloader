#include "int_bootloader.h"
#include <string.h>

static uint8_t uart_rec_buff[PACKET_DATA_SIZE] = {0};

volatile uint8_t uart_rx_ready = 0U;
volatile uint32_t uart_total_len = 0U;

typedef void (*app_func_t)(void);

void bootloader_init(void)
{
    HAL_StatusTypeDef status;

    uart_rx_ready = 0U;
    uart_total_len = 0U;

    BootCache_Init();

    status = Flash_EraseApp();
    if (status != HAL_OK)
    {
        uart_rx_ready = 2U;
        return;
    }

    __HAL_UART_CLEAR_IDLEFLAG(&huart1);
    __HAL_UART_CLEAR_OREFLAG(&huart1);

    HAL_UARTEx_ReceiveToIdle_IT(&huart1,
                               uart_rec_buff,
                               PACKET_DATA_SIZE);
}

void boot_loder_jump_to_app(void)
{
    uint32_t app_stack_addr;
    uint32_t app_reset_addr;

    app_func_t Jump_to_App;
	
		printf("start jump\r\n");

    /*
     *获取APP的栈顶地址和复位处理函数地址 
     */
    app_stack_addr = *(volatile uint32_t*)APP_START_ADDR;
    app_reset_addr = *(volatile uint32_t*)(APP_START_ADDR + 4);

    /*app的栈顶地址必须在SRAM范围内*/
    if ((app_stack_addr < SRAM1_BASE) ||
        (app_stack_addr >= (SRAM2_BASE + 0x4000)))
    {
				printf("app_stack_error\r\n");
        return;
    }

    /*复位处理函数地址必须在APP的Flash范围内*/
    if ((app_reset_addr < APP_START_ADDR) ||
    (app_reset_addr >= FLASH_END_ADDR))
    {
				printf("reset_error\r\n");
        return;
    }

    /*注销bootloader*/
    /*关闭中断*/
    __disable_irq();

    HAL_DeInit();

    /*设置主堆栈指针*/
    __set_MSP(app_stack_addr);

    /*重定向中断向量表*/
    SCB->VTOR = APP_START_ADDR;

    /*跳转到APP的复位处理函数*/
    

    Jump_to_App = (app_func_t)app_reset_addr;

    Jump_to_App();
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
