#include "int_bootloader.h"
#include "boot_protocol.h"
#include "boot_crc.h"
#include <string.h>

#define BOOT_COPY_BUFFER_SIZE  256U /*复制缓冲区大小*/

static uint8_t boot_copy_buffer[BOOT_COPY_BUFFER_SIZE] = {0};

static uint8_t uart_rec_buff[BOOT_FRAME_MAX_SIZE] = {0};

/*
 * 协议帧接收器。
 * 用于跨多次UART回调拼接一张完整协议帧。
 */
static Boot_RxContextTypeDef protocol_rx_context;

/*
 * 完整协议帧等待主循环处理标志。
 * 在UART中断中置1，在主循环处理完成后清0。
 */
static volatile uint8_t protocol_frame_pending = 0U;

/*
 * 当前协议升级状态。
 */
static Boot_StateTypeDef protocol_update_state = BOOT_IDLE;

/*
 * 保存当前START帧提供的升级信息。
 */
static Boot_StartInfoTypeDef protocol_update_info =
{
    UPDATE_NONE,
    0U,
    0U
};

/*
 * 当前已经成功写入目标区的BIN字节数。
 */
static uint32_t protocol_received_size = 0U;

/*
 * 下一张DATA帧应该携带的包序号。
 * 第一包从0开始。
 */
static uint32_t protocol_expected_sequence = 0U;


/*
 * 协议接收过程发生长度错误标志。
 */
static volatile uint8_t protocol_rx_error_pending = 0U;

typedef void (*app_func_t)(void);


void bootloader_init(void)
{

    /*初始化帧接收器*/
    Boot_RxInit(&protocol_rx_context);

    protocol_rx_error_pending = 0;
    protocol_frame_pending = 0;

    protocol_update_state = BOOT_IDLE;

    protocol_update_info.target = UPDATE_NONE;
    protocol_update_info.image_size = 0U;
    protocol_update_info.image_crc = 0U;

    protocol_received_size = 0U;
    protocol_expected_sequence = 0U;


    memset(uart_rec_buff, 0, sizeof(uart_rec_buff));

    __HAL_UART_CLEAR_IDLEFLAG(&huart1);
    __HAL_UART_CLEAR_OREFLAG(&huart1);

    HAL_UARTEx_ReceiveToIdle_IT(&huart1,
                               uart_rec_buff,
                               BOOT_FRAME_MAX_SIZE);
    
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
    uint32_t start_addr;

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

            start_addr = APP_A_ADDR;
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

            start_addr = APP_B_ADDR;
            break;

        default:
            return HAL_ERROR;
    }

    BootCache_Init(start_addr);
    return HAL_OK;
}

/**
 * @brief 结束当前协议帧处理，重新开启UART接收。
 */
static void Boot_ProtocolReceiveRestart(void)
{
    /*
     * 清除当前完整帧的接收进度。
     */
    Boot_RxInit(&protocol_rx_context);

    /*
     * 当前完整帧已经处理完毕。
     */
    protocol_frame_pending = 0U;
    memset(uart_rec_buff,0,sizeof(uart_rec_buff));

    /*
     * 重新开启UART空闲中断接收。
     */
    HAL_UARTEx_ReceiveToIdle_IT(&huart1,uart_rec_buff, BOOT_FRAME_MAX_SIZE);
}



/**
 * @brief 构造并发送一张ACK/NACK应答帧。
 *
 * @param response_cmd 应答类型，只能是CMD_ACK或CMD_NACK。
 * @param request_cmd  本次应答对应的原始请求命令。
 * @param result       请求处理结果。
 * @param value        附加信息，例如包序号或期望包序号。
 *
 * @return HAL_OK表示发送成功，其余表示构造或发送失败。
 */
static HAL_StatusTypeDef Boot_SendResponse(
    Boot_CmdTypeDef response_cmd,
    Boot_CmdTypeDef request_cmd,
    Boot_ResultTypeDef result,
    uint32_t value)
{
    uint8_t response_frame[BOOT_RESPONSE_FRAME_SIZE];
    uint16_t response_len;

    /*
     * 根据应答内容构造完整的14字节协议帧。
     */
    response_len = Boot_BuildResponseFrame(
        response_frame,
        sizeof(response_frame),
        response_cmd,
        request_cmd,
        result,
        value);

    /*
     * 返回0说明参数错误，应答帧构造失败。
     */
    if (response_len == 0U)
    {
        return HAL_ERROR;
    }

    /*
     * 当前函数在主循环中执行，不在UART中断中执行，
     * 因此这里可以暂时使用阻塞发送。
     */
    return HAL_UART_Transmit(
        &huart1,
        response_frame,
        response_len,
        100U);
}

/**
 * @brief 处理一张START升级帧。
 * @param frame     完整协议帧地址。
 * @param frame_len 完整协议帧长度。
 */
static void Boot_HandleStartFrame(uint8_t *frame, uint16_t frame_len)
{
    Boot_StartInfoTypeDef start_info;
    HAL_StatusTypeDef status;

    uint32_t target_region_size;
    uint32_t run_region_size;

    /*
     * 检查START帧的长度、CRC和DATA内容。
     */
    if (Boot_ParseStartFrame(frame, frame_len, &start_info) == 0U)
    {
        printf("START_FRAME_ERROR\r\n");
    /*
     * START帧长度、内容或者CRC错误。
     */
    (void)Boot_SendResponse(
        CMD_NACK,
        CMD_START_UPDATE,
        BOOT_RESULT_FRAME_ERROR,
        0U);
        return;
    }

    printf("START_FRAME_OK\r\n");
    printf("TARGET:%u\r\n",          (unsigned int)start_info.target);
    printf("IMAGE_SIZE:%lu\r\n",(unsigned long)start_info.image_size);
    printf("IMAGE_CRC:0x%04X\r\n",(unsigned int)start_info.image_crc);

    /*
     * 当前已经处于升级接收状态，
     * 不允许再次执行START擦除。
     */
    if (protocol_update_state == BOOT_RECEIVE)
    {
        printf("UPDATE_BUSY\r\n");
    /*
     * 当前状态不允许再次执行START。
     * value返回当前升级状态。
     */
    (void)Boot_SendResponse(
        CMD_NACK,
        CMD_START_UPDATE,
        BOOT_RESULT_STATE_ERROR,
        (uint32_t)protocol_update_state);

        return;
    }

    /*
     * 根据目标计算A区或者B区容量。
     */
    if (start_info.target == UPDATE_APP_A)
    {
        target_region_size = APP_A_END_ADDR - APP_A_ADDR + 1U;
    }
    else
    {
        target_region_size = APP_B_END_ADDR - APP_B_ADDR + 1U;
    }

    /*
     * 计算运行区容量。
     */
    run_region_size = APP_RUN_END_ADDR - APP_RUN_ADDR + 1U;

    /*
     * BIN文件不能超过存储区和运行区。
     */
    if ((start_info.image_size > target_region_size) ||
        (start_info.image_size > run_region_size))
    {
        printf("IMAGE_TOO_LARGE\r\n");
        /*
     * START声明的文件超过Flash区域容量。
     * value返回上位机声明的文件大小。
     */
    (void)Boot_SendResponse(
        CMD_NACK,
        CMD_START_UPDATE,
        BOOT_RESULT_IMAGE_TOO_LARGE,
        start_info.image_size);
        return;
    }

    /*
     * 准备开始擦除目标区。
     */
    if (start_info.target == UPDATE_APP_A)
    {
        printf("ERASING_A\r\n");
    }
    else
    {
        printf("ERASING_B\r\n");
    }

    /*
     * 擦除目标区并初始化Flash写入缓存。
     */
    status = Boot_StartUpdate(start_info.target);

    if (status != HAL_OK)
    {
        protocol_update_state = BOOT_ERROR;
        printf("ERASING_FAILED\r\n");
        /*
        * A区或者B区擦除失败。
        * value返回HAL状态码。
        */
        (void)Boot_SendResponse(
            CMD_NACK,
            CMD_START_UPDATE,
            BOOT_RESULT_FLASH_ERROR,
            (uint32_t)status);

        return;
    }

    /*
     * 擦除成功，保存START帧中的升级信息。
     */
    protocol_update_info = start_info;
    protocol_update_state = BOOT_RECEIVE;

    /*
    * 新升级开始，接收计数和包序号从0开始。
    */
    protocol_received_size = 0U;
    protocol_expected_sequence = 0U;


    /*
     * 进入对应的A/B接收状态。
     */
    if (start_info.target == UPDATE_APP_A)
    {
        printf("RECEIVE_A\r\n");
    }
    else
    {
        printf("RECEIVE_B\r\n");
    }

    printf("START_UPDATE_OK\r\n");
    printf("READY_SIZE:%lu\r\n", (unsigned long)protocol_update_info.image_size);
    /*
    * START处理成功。
    *
    * value返回本次升级目标：
    * 1表示A区，2表示B区。
    */
    (void)Boot_SendResponse(
        CMD_ACK,
        CMD_START_UPDATE,
        BOOT_RESULT_OK,
        (uint32_t)protocol_update_info.target);
}

/**
 * @brief 处理一张DATA固件数据帧。
 * @param frame     完整协议帧地址。
 * @param frame_len 完整协议帧长度。
 */
static void Boot_HandleDataFrame(uint8_t *frame,uint16_t frame_len)
{
    Boot_DataInfoTypeDef data_info;
    HAL_StatusTypeDef status;

    uint32_t remaining_size;

    if(protocol_update_state != BOOT_RECEIVE)
    {
        printf("DATA_without_start\r\n");

    (void)Boot_SendResponse(
        CMD_NACK,
        CMD_DATA_PACKET,
        BOOT_RESULT_STATE_ERROR,
        (uint32_t)protocol_update_state);
        return;
    }

        /*
     * 检查DATA帧的长度、CMD和帧CRC，
     * 并解析数据地址、数据长度和包序号。
     */
    if (Boot_ParseDataFrame(frame, frame_len, &data_info) == 0U)
    {
        printf("DATA_FRAME_ERROR\r\n");
        (void)Boot_SendResponse(
        CMD_NACK,
        CMD_DATA_PACKET,
        BOOT_RESULT_FRAME_ERROR,
        0U);
        return;
    }
    /*
     * 检查当前数据包序号。
     *
     * 第一包必须是0，成功后期望序号加1。
     */
    if (data_info.sequence != protocol_expected_sequence)
    {
        printf("SEQUENCE_ERROR\r\n");
        printf("EXPECTED:%lu\r\n",(unsigned long)protocol_expected_sequence);
        printf("RECEIVED:%lu\r\n",(unsigned long)data_info.sequence);

       /*
        * value返回Bootloader当前期望的包序号。
        * 上位机可以据此重发正确的数据包。
        */
        (void)Boot_SendResponse(
            CMD_NACK,
            CMD_DATA_PACKET,
            BOOT_RESULT_SEQUENCE_ERROR,
            protocol_expected_sequence);
        return;
    }

     /*
     * 防止减法发生无符号下溢。
     */
    if (protocol_received_size > protocol_update_info.image_size)
    {
        protocol_update_state = BOOT_ERROR;
        printf("RECEIVED_SIZE_ERROR\r\n");

        (void)Boot_SendResponse(
        CMD_NACK,
        CMD_DATA_PACKET,
        BOOT_RESULT_IMAGE_SIZE_ERROR,
        protocol_received_size);
        return;
    }

    /*
     * 计算当前固件还剩多少字节没有接收。
     */
    remaining_size = protocol_update_info.image_size - protocol_received_size;

    /*
     * 当前DATA包不能超过START声明的文件大小。
     */
    if ((uint32_t)data_info.data_len > remaining_size)
    {
        printf("DATA_TOO_LARGE\r\n");
        printf("REMAINING:%lu\r\n", (unsigned long)remaining_size);

        (void)Boot_SendResponse(
            CMD_NACK,
            CMD_DATA_PACKET,
            BOOT_RESULT_DATA_TOO_LARGE,
            remaining_size);
        return;
    }
    
    /*
     * 将当前DATA中的BIN数据送进4字节Flash缓存。
     *
     * data_info.data指向frame中的DATA起始地址，
     * data_info.data_len是本包真实BIN字节数。
     */
    status = Boot_WriteCache(data_info.data,data_info.data_len);
    if (status != HAL_OK)
    {
        protocol_update_state = BOOT_ERROR;
        printf("FLASH_WRITE_FAILED\r\n");

        (void)Boot_SendResponse(
        CMD_NACK,
        CMD_DATA_PACKET,
        BOOT_RESULT_FLASH_ERROR,
        data_info.sequence);

        return;
    }

    /*
     * Flash写入成功后再更新计数。
     */
    protocol_received_size += data_info.data_len;
    protocol_expected_sequence++;

    printf("DATA_FRAME_OK\r\n");
    printf("SEQ:%lu\r\n",(unsigned long)data_info.sequence);
    printf("DATA_LEN:%u\r\n",(unsigned int)data_info.data_len);
    printf("RECEIVED_SIZE:%lu\r\n",(unsigned long)protocol_received_size);

    /*
    * 只有数据成功写入Flash、计数和期望序号更新后，
    * 才向上位机发送ACK。
    *
    * value返回刚刚成功写入的数据包序号。
    */
    (void)Boot_SendResponse(
        CMD_ACK,
        CMD_DATA_PACKET,
        BOOT_RESULT_OK,
        data_info.sequence);
}

/**
 * @brief 处理一张END升级结束帧。
 */
static void Boot_HandleEndFrame(uint8_t *frame, uint16_t frame_len)
{
    HAL_StatusTypeDef status;
    uint32_t packet_count;
    uint32_t image_addr;
    uint16_t calculated_image_crc;

    /*
     * 必须先成功处理START，
     * 并处于DATA接收状态，才允许处理END。
     */
    if (protocol_update_state != BOOT_RECEIVE)
    {
        printf("END_WITHOUT_START\r\n");

        (void)Boot_SendResponse(
            CMD_NACK,
            CMD_END_UPDATE,
            BOOT_RESULT_STATE_ERROR,
            (uint32_t)protocol_update_state);

        return;
    }

    /*
     * 检查END帧的CMD、长度和帧CRC，
     * 并从RESERVE字段解析总包数。
     */
    if (Boot_ParseEndFrame(frame, frame_len, &packet_count) == 0U)
    {
        printf("END_FRAME_ERROR\r\n");

        (void)Boot_SendResponse(
            CMD_NACK,
            CMD_END_UPDATE,
            BOOT_RESULT_FRAME_ERROR,
            0U);
        return;
    }

    printf("END_FRAME_OK\r\n");

    printf("PACKET_COUNT:%lu\r\n",(unsigned long)packet_count);

    /*
     * protocol_expected_sequence从0开始，
     * 每成功接收一包就加1。
     *
     * 因此它当前也等于已经成功接收的总包数。
     */
    if (packet_count != protocol_expected_sequence)
    {
        printf("PACKET_COUNT_ERROR\r\n");
        printf("EXPECTED_PACKETS:%lu\r\n",(unsigned long) protocol_expected_sequence);
        printf("RECEIVED_PACKETS:%lu\r\n",(unsigned long) packet_count);

        /*
        * value返回Bootloader实际成功接收的包数。
        */
        (void)Boot_SendResponse(
            CMD_NACK,
            CMD_END_UPDATE,
            BOOT_RESULT_PACKET_COUNT_ERROR,
            protocol_expected_sequence);
        return;
    }

    /*
     * 实际接收的BIN字节数必须等于
     * START帧声明的整个文件大小。
     */
    if (protocol_received_size != protocol_update_info.image_size)
    {
        printf("IMAGE_SIZE_ERROR\r\n");
        printf("EXPECTED_SIZE:%lu\r\n", (unsigned long) protocol_update_info.image_size);
        printf("RECEIVED_SIZE:%lu\r\n", (unsigned long) protocol_received_size);

        /*
        * value返回实际接收到的BIN字节数。
        */
        (void)Boot_SendResponse(
            CMD_NACK,
            CMD_END_UPDATE,
            BOOT_RESULT_IMAGE_SIZE_ERROR,
            protocol_received_size);
            return;
    }
    printf("IMAGE_SIZE_OK\r\n");

    /*
     * 包数和文件大小均正确，
     * 开始进行最终固件检查。
     */
    protocol_update_state = BOOT_CHECK;

    /*
     * 如果整个BIN长度不是4的倍数，
     * 把缓存中最后1～3字节补0xFF并写入Flash。
     * 如果缓存刚好为空，本函数直接返回HAL_OK。
     */
    status = Boot_FlushCache();

    if (status != HAL_OK)
    {
        protocol_update_state = BOOT_ERROR;
        printf("FLUSH_CACHE_FAILED\r\n");

        (void)Boot_SendResponse(
            CMD_NACK,
            CMD_END_UPDATE,
            BOOT_RESULT_FLASH_ERROR,
            (uint32_t)status);
        return;
    }

    /*
     * 根据START帧中的目标，
     * 确定刚接收的固件位于A区还是B区。
     */
    if (protocol_update_info.target == UPDATE_APP_A)
    {
        image_addr = APP_A_ADDR;
    }
    else if (protocol_update_info.target == UPDATE_APP_B)
    {
        image_addr = APP_B_ADDR;
    }
    else
    {
        protocol_update_state = BOOT_ERROR;
        printf("UPDATE_TARGET_ERROR\r\n");
        (void)Boot_SendResponse(
            CMD_NACK,
            CMD_END_UPDATE,
            BOOT_RESULT_STATE_ERROR,
            (uint32_t)protocol_update_info.target);
        return;
    }

    /*
     * 直接读取Flash中的真实BIN数据，
     * 重新计算整个文件的Modbus CRC16。
     *
     * 这里只计算真实image_size字节，
     * 不计算最后补齐的0xFF。
     */
    calculated_image_crc =Boot_CRC16_Modbus((const uint8_t *)image_addr, protocol_update_info.image_size);

    printf("EXPECTED_IMAGE_CRC:0x%04X\r\n",(unsigned int) protocol_update_info.image_crc);
    printf("CALCULATED_IMAGE_CRC:0x%04X\r\n",(unsigned int) calculated_image_crc);

    /*
     * 与START帧中上位机提供的整个BIN CRC比较。
     */
    if (calculated_image_crc != protocol_update_info.image_crc)
    {
        protocol_update_state = BOOT_ERROR;
        printf("IMAGE_CRC_ERROR\r\n");

        /*
        * value的低16位返回实际计算出的固件CRC。
        */
        (void)Boot_SendResponse(
            CMD_NACK,
            CMD_END_UPDATE,
            BOOT_RESULT_IMAGE_CRC_ERROR,
            (uint32_t)calculated_image_crc);
        return;
    }

    /*
     * 整个升级文件校验成功。
     */
    protocol_update_state = BOOT_READY;
    printf("IMAGE_CRC_OK\r\n");
    printf("UPDATE_READY\r\n");

    /*
    * 固件大小、包数和整个BIN CRC全部正确。
    * value返回成功接收的DATA总包数。
    */
    (void)Boot_SendResponse(
        CMD_ACK,
        CMD_END_UPDATE,
        BOOT_RESULT_OK,
        protocol_expected_sequence);
}

/**
 * @brief 获取并处理一张完整协议帧。
 */
static void Boot_ProcessProtocolFrame(void)
{
    uint8_t *frame;
    uint16_t frame_len;
    uint16_t cmd;

    frame = NULL;
    frame_len = 0U;

    /*
     * 从协议接收器获取完整帧,获取到frame里面去。
     */
    if (Boot_RxGetFrame(&protocol_rx_context, &frame, &frame_len) == 0U)
    {
        printf("GET_FRAME_FAILED\r\n");
        Boot_ProtocolReceiveRestart();
        return;
    }

    /*
     * 读取当前协议帧的CMD。
     */
    cmd = Boot_ParseCmd(frame);

    printf("FRAME_READY\r\n");
    printf("CMD:0x%04X\r\n",(unsigned int)cmd);
    printf("LEN:%u\r\n",(unsigned int)frame_len);

    /*
     * 根据CMD将协议帧分发给对应处理函数。
     */
    switch ((Boot_CmdTypeDef)cmd)
    {
        case CMD_START_UPDATE:
            Boot_HandleStartFrame(frame, frame_len);
            break;

        case CMD_DATA_PACKET:
            Boot_HandleDataFrame(frame, frame_len);
            break;

        case CMD_END_UPDATE:
            Boot_HandleEndFrame(frame, frame_len);
            break;

        default:
            printf("UNKNOWN_CMD:0x%04X\r\n",(unsigned int)cmd);
            /*
            * 收到当前Bootloader不支持的命令。
            *
            * request_cmd保留上位机发送的原始命令，
            * 方便上位机确认是哪一条命令不被支持。
            */
            (void)Boot_SendResponse(
                CMD_NACK,
                (Boot_CmdTypeDef)cmd,
                BOOT_RESULT_UNKNOWN_CMD,
                0U);
            break;
    }

    /*
     * 当前协议帧处理完成，重新开启UART接收。
     */
    Boot_ProtocolReceiveRestart();
}


void Bootloader_Process(void)
{
    /*
     * 处理UART拼帧错误。
     */
    if (protocol_rx_error_pending != 0U)
    {
        protocol_rx_error_pending = 0U;

        printf("PROTOCOL_RX_ERROR\r\n");
    }

    /*
     * 处理已经接收完成的协议帧。
     */
    if (protocol_frame_pending != 0U)
    {
        Boot_ProcessProtocolFrame();
    }
}


void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart,uint16_t Size)
{{
    Boot_RxResultTypeDef result;
    uint16_t i;

    if ((huart == NULL) ||
        (huart->Instance != USART1))
    {
        return;
    }

    /*
     * 将本次UART收到的Size个字节，
     * 逐个送入协议帧接收器。
     */
    for (i = 0U; i < Size; i++)
    {
        result = Boot_RxInputByte(&protocol_rx_context, uart_rec_buff[i]);

        /*
         * 前4字节中的TOTAL_LEN非法。
         */
        if (result == BOOT_RX_FRAME_ERROR)
        {
            protocol_rx_error_pending = 1U;
            break;
        }

        /*
         * 已经接收到一张完整协议帧。
         */
        if (result == BOOT_RX_FRAME_READY)
        {
            protocol_frame_pending = 1U;
            break;
        }
    }

    /*
     * 没有完整帧时继续接收。
     *
     * 如果完整帧已经就绪，就暂时停止重新接收，
     * 等Bootloader_Process处理完成后再开启。
     */
    if (protocol_frame_pending == 0U)
    {
        memset(uart_rec_buff,0,sizeof(uart_rec_buff));

        HAL_UARTEx_ReceiveToIdle_IT(&huart1,uart_rec_buff,PACKET_DATA_SIZE);
    }
}}
