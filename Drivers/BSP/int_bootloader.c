#include "int_bootloader.h"
#include "boot_protocol.h"
#include <string.h>



#define BOOT_SELECT_HEADER_1    0x55U
#define BOOT_SELECT_HEADER_2    0xAAU

#define BOOT_SELECT_APP_A       0x01U
#define BOOT_SELECT_APP_B       0x02U

#define BOOT_SELECT_CMD_SIZE    3U
#define BOOT_COPY_BUFFER_SIZE  256U /*复制缓冲区大小*/

static uint8_t boot_copy_buffer[BOOT_COPY_BUFFER_SIZE] = {0};
static uint8_t uart_rec_buff[PACKET_DATA_SIZE] = {0};

static uint32_t update_addr = 0U;       /*当前更新地址*/
static uint32_t update_end_addr = 0U; /*当前更新结束地址*/

static volatile uint32_t update_last_rx_tick = 0U; 
static volatile uint8_t update_has_data = 0U;

static Update_TargetTypeDef update_target = UPDATE_NONE;

static volatile Boot_ReceiveStateTypeDef boot_receive_state = BOOT_WAIT_COMMAND;

static volatile Update_TargetTypeDef pending_target =UPDATE_NONE;

volatile uint8_t uart_rx_ready = 0U;
volatile uint32_t uart_total_len = 0U;

typedef void (*app_func_t)(void);

void bootloader_init(void)
{
    uart_rx_ready = 0U;
    uart_total_len = 0U;

    update_addr = 0U;
    update_target = UPDATE_NONE;
    pending_target = UPDATE_NONE;

    boot_receive_state = BOOT_WAIT_COMMAND;

    memset(uart_rec_buff, 0, sizeof(uart_rec_buff));

    __HAL_UART_CLEAR_IDLEFLAG(&huart1);
    __HAL_UART_CLEAR_OREFLAG(&huart1);

    HAL_UARTEx_ReceiveToIdle_IT(&huart1,
                               uart_rec_buff,
                               PACKET_DATA_SIZE);

    printf("WAIT_COMMAND\r\n");
}

void Boot_JumpToApp(void)
{
    uint32_t app_stack_addr;
    uint32_t app_reset_addr;

    app_func_t jump_to_app;


    app_stack_addr = *(volatile uint32_t*)APP_RUN_ADDR;
    app_reset_addr = *(volatile uint32_t*)(APP_RUN_ADDR + 4U);


    if((app_stack_addr < SRAM1_BASE) ||
       (app_stack_addr >= SRAM2_BASE + 0x4000))
    {
        return;
    }


    if((app_reset_addr < APP_RUN_ADDR) ||
       (app_reset_addr >= APP_RUN_END_ADDR))
    {
        return;
    }


    if((app_reset_addr & 0x01) == 0)
    {
        return;
    }


    jump_to_app = (app_func_t)app_reset_addr;

    __disable_irq();


    SysTick->CTRL = 0;
    SysTick->LOAD = 0;
    SysTick->VAL  = 0;


    HAL_DeInit();


    for(uint8_t i=0;i<8;i++)
    {
        NVIC->ICER[i] = 0xFFFFFFFF;
        NVIC->ICPR[i] = 0xFFFFFFFF;
    }


    SCB->VTOR = APP_RUN_ADDR;

    __DSB();
    __ISB();

    __set_MSP(app_stack_addr);

    __DSB();
    __ISB();

    /* PRIMASK不会自动恢复，必须重新开启 */
    __enable_irq();

    jump_to_app();

    while (1)
    {
    }
}

HAL_StatusTypeDef Boot_CopyToRun(uint32_t source_addr, uint32_t image_size)
{
    HAL_StatusTypeDef status;
    uint32_t source_end_addr;
    uint32_t source_region_size;
    uint32_t run_region_size;
    uint32_t copied_size;
    uint32_t data_size;
    uint32_t write_size;
    uint32_t i;

    /**
     * 当前只允许A区或者B区搬运
     */

     if(source_addr == APP_A_ADDR)
     {
        source_end_addr = APP_A_END_ADDR;
     }
     else if(source_addr == APP_B_ADDR)
     {
        source_end_addr = APP_B_END_ADDR;
     }
     else
     {
        return HAL_ERROR;
     }
     /*获取源区和运行区大小*/
     source_region_size = source_end_addr - source_addr + 1U;
     run_region_size = APP_RUN_END_ADDR - APP_RUN_ADDR + 1U;

         /*
     * 检查固件长度是否合法。
     */
    if ((image_size == 0U) ||
        (image_size > source_region_size) ||
        (image_size > run_region_size))
    {
        return HAL_ERROR;
    }
    
        /*
     * 先擦除整个运行区：
     * Sector 5：0x08020000～0x0803FFFF
     * Sector 6：0x08040000～0x0805FFFF
     */
    status = Flash_EraseSector(FLASH_SECTOR_5);
    if (status != HAL_OK)
    {
        return status;
    }

    status = Flash_EraseSector(FLASH_SECTOR_6);
    if (status != HAL_OK)
    {
        return status;
    }

    copied_size = 0U;

    while (copied_size < image_size)
    {
        /*
         * 计算本次实际复制的数据长度。
         */
        data_size = image_size - copied_size;

        if (data_size > BOOT_COPY_BUFFER_SIZE)
        {
            data_size = BOOT_COPY_BUFFER_SIZE;
        }

        /*
         * Flash_Write要求长度必须为4的倍数。
         */
        write_size = (data_size + 3U) & (~3U);

        /*
         * 最后一包不足4字节时使用0xFF填充。
         */
        memset(boot_copy_buffer,
               0xFF,
               sizeof(boot_copy_buffer));

        /*
         * 先从A/B区复制到RAM。
         */
        memcpy(boot_copy_buffer,
               (const void *)(source_addr + copied_size),
               data_size);

        /*
         * 再从RAM写入运行区。
         */
        status = Flash_Write(
            APP_RUN_ADDR + copied_size,
            boot_copy_buffer,
            write_size);

        if (status != HAL_OK)
        {
            return status;
        }

        copied_size += data_size;
    }

    /*
     * 逐字节比较源区和运行区。
     * 这里只比较真实固件长度，不比较最后补齐的0xFF。
     */
    for (i = 0U; i < image_size; i++)
    {
        if (*(volatile uint8_t *)(source_addr + i) !=
            *(volatile uint8_t *)(APP_RUN_ADDR + i))
        {
            return HAL_ERROR;
        }
    }

    return HAL_OK;
}

static HAL_StatusTypeDef Boot_StartUpdate(Update_TargetTypeDef target)
{
    HAL_StatusTypeDef status;

    switch(target)
    {
        case UPDATE_APP_A:
            status = Flash_EraseSector(FLASH_SECTOR_7);
            if (status != HAL_OK)
            {
                return status;
            }

            status = Flash_EraseSector(FLASH_SECTOR_8);
            if (status != HAL_OK)
            {
                return status;
            }

            update_addr = APP_A_ADDR;
            update_end_addr = APP_A_END_ADDR;
            break;

        case UPDATE_APP_B:
            status = Flash_EraseSector(FLASH_SECTOR_9);
            if (status != HAL_OK)
            {
                return status;
            }

            status = Flash_EraseSector(FLASH_SECTOR_10);
            if (status != HAL_OK)
            {
                return status;
            }

            update_addr = APP_B_ADDR;
            update_end_addr = APP_B_END_ADDR;
            break;

        default:
            return HAL_ERROR;
    }

    BootCache_Init(update_addr);

    update_target = target;

    uart_total_len = 0;
    uart_rx_ready = 0;

    update_has_data = 0;
    update_last_rx_tick = 0;

    return HAL_OK;
}

void Bootloader_Process(void)
{
    Update_TargetTypeDef target;
    HAL_StatusTypeDef status;
    uint32_t finished_size;

    /*
     * 处理A/B选择请求。
     */
    if (pending_target != UPDATE_NONE)
    {
        target = pending_target;
        pending_target = UPDATE_NONE;

        status = Boot_StartUpdate(target);

        if (status != HAL_OK)
        {
            boot_receive_state =
                BOOT_RECEIVE_ERROR;

            uart_rx_ready = 2U;

            printf("ERASE_FAILED\r\n");
            return;
        }

        if (target == UPDATE_APP_A)
        {
            boot_receive_state = BOOT_RECEIVE_A;

            printf("READY_A\r\n");
        }
        else if (target == UPDATE_APP_B)
        {
            boot_receive_state = BOOT_RECEIVE_B;

            printf("READY_B\r\n");
        }

        return;
    }

    /*
     * 已经收到数据，并且500ms没有新数据，
     * 判定BIN文件发送完成。
     */
    if (((boot_receive_state == BOOT_RECEIVE_A) ||
         (boot_receive_state == BOOT_RECEIVE_B)) &&
        (update_has_data != 0U) &&
        ((HAL_GetTick() - update_last_rx_tick) >=
         500U))
    {
        /*
         * 先改变状态，防止继续写入。
         */
        boot_receive_state = BOOT_FINISHING;

        finished_size = uart_total_len;

        status = Boot_FlushCache();

        if (status == HAL_OK)
        {
            printf("UPDATE_FINISH:%lu\r\n",
                   finished_size);

            uart_rx_ready = 1U;
        }
        else
        {
            printf("FLUSH_FAILED\r\n");

            uart_rx_ready = 2U;
        }

        update_target = UPDATE_NONE;

        update_addr = 0U;
        update_end_addr = 0U;

        update_last_rx_tick = 0U;
        update_has_data = 0U;

        /*
         * 重新等待下一次A/B命令。
         */
        boot_receive_state = BOOT_WAIT_COMMAND;

        printf("WAIT_COMMAND\r\n");
    }
}

void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart,
                                uint16_t Size)
{
    HAL_StatusTypeDef status;
    uint32_t region_size;

    if ((huart == NULL) ||
        (huart->Instance != USART1))
    {
        return;
    }

    /*
     * 等待命令状态：
     * 只识别55 AA 01和55 AA 02。
     */
    if (boot_receive_state == BOOT_WAIT_COMMAND)
    {
        if ((Size == BOOT_SELECT_CMD_SIZE) &&
            (uart_rec_buff[0] ==
                BOOT_SELECT_HEADER_1) &&
            (uart_rec_buff[1] ==
                BOOT_SELECT_HEADER_2))
        {
            if (uart_rec_buff[2] ==
                BOOT_SELECT_APP_A)
            {
                pending_target = UPDATE_APP_A;
                boot_receive_state = BOOT_ERASING;
            }
            else if (uart_rec_buff[2] ==
                     BOOT_SELECT_APP_B)
            {
                pending_target = UPDATE_APP_B;
                boot_receive_state = BOOT_ERASING;
            }
        }
    }
    /*
     * 接收A/B原始BIN。
     */
    else if ((boot_receive_state == BOOT_RECEIVE_A) ||
             (boot_receive_state == BOOT_RECEIVE_B))
    {
        if (Size > 0U)
        {
            region_size =
                update_end_addr - update_addr + 1U;

            /*
             * 防止BIN超过A/B区域。
             */
            if ((uart_total_len > region_size) ||
                ((uint32_t)Size >
                 (region_size - uart_total_len)))
            {
                boot_receive_state =
                    BOOT_RECEIVE_ERROR;

                uart_rx_ready = 2U;

                printf("IMAGE_TOO_LARGE\r\n");
            }
            else
            {
                /*
                 * 当前处于接收模式，
                 * 所有收到的字节都作为BIN数据写入。
                 */
                status = Boot_WriteCache(
                    uart_rec_buff,
                    Size);

                if (status == HAL_OK)
                {
                    uart_total_len += Size;

                    update_last_rx_tick =
                        HAL_GetTick();

                    update_has_data = 1U;
                    uart_rx_ready = 1U;
                }
                else
                {
                    boot_receive_state =
                        BOOT_RECEIVE_ERROR;

                    uart_rx_ready = 2U;

                    printf("FLASH_WRITE_FAILED\r\n");
                }
            }
        }
    }

    memset(uart_rec_buff, 0, sizeof(uart_rec_buff));

    HAL_UARTEx_ReceiveToIdle_IT(&huart1,
                               uart_rec_buff,
                               PACKET_DATA_SIZE);
}