#include "int_bootloader.h"
#include "boot_protocol.h"
#include "boot_crc.h"
#include <string.h>
#include "boot_flag.h"

#define BOOT_COPY_BUFFER_SIZE 256U /*复制缓冲区大小*/

/*
 * Bootloader上电后等待升级命令的时间。
 * 3000ms等于3秒。
 */
#define BOOT_WAIT_TIMEOUT_MS 3000U

/*
 * 本次上电是否已经执行过启动决定。
 *
 * 0：还在等待；
 * 1：已经尝试过安装或跳转。
 */
static uint8_t boot_start_decision_done = 0U;

/*Bootloader开始等待升级命令时的系统毫秒数。*/
static uint32_t boot_wait_start_tick = 0U;

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
static Boot_StartInfoTypeDef protocol_update_info = {UPDATE_NONE, 0U, 0U};

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

    /*从当前时刻开始计算Bootloader等待时间。*/
    boot_wait_start_tick = HAL_GetTick();

    /*本次上电还没有做启动决定。*/
    boot_start_decision_done = 0U;

    memset(uart_rec_buff, 0, sizeof(uart_rec_buff));

    __HAL_UART_CLEAR_IDLEFLAG(&huart1);
    __HAL_UART_CLEAR_OREFLAG(&huart1);

    HAL_UARTEx_ReceiveToIdle_IT(&huart1, uart_rec_buff, BOOT_FRAME_MAX_SIZE);

    if (Boot_FlagInit() == HAL_OK)
    {
        printf("BOOT_FLAG_INIT_OK\r\n");
    }
    else
    {
        printf("BOOT_FLAG_INIT_ERROR\r\n");
    }
}

void Boot_JumpToApp(void)
{
    uint32_t app_stack_addr;
    uint32_t app_reset_addr;

    app_func_t jump_to_app;

    app_stack_addr = *(volatile uint32_t *)APP_RUN_ADDR;
    app_reset_addr = *(volatile uint32_t *)(APP_RUN_ADDR + 4U);

    if ((app_stack_addr < SRAM1_BASE) ||
        (app_stack_addr >= SRAM2_BASE + 0x4000))
    {
        return;
    }

    if ((app_reset_addr < APP_RUN_ADDR) ||
        (app_reset_addr >= APP_RUN_END_ADDR))
    {
        return;
    }

    if ((app_reset_addr & 0x01) == 0)
    {
        return;
    }

    jump_to_app = (app_func_t)app_reset_addr;

    __disable_irq();

    SysTick->CTRL = 0;
    SysTick->LOAD = 0;
    SysTick->VAL = 0;

    HAL_DeInit();

    for (uint8_t i = 0; i < 8; i++)
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

    if (source_addr == APP_A_ADDR)
    {
        source_end_addr = APP_A_END_ADDR;
    }
    else if (source_addr == APP_B_ADDR)
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
        memset(boot_copy_buffer, 0xFF, sizeof(boot_copy_buffer));

        /*
         * 先从A/B区复制到RAM。
         */
        memcpy(boot_copy_buffer, (const void *)(source_addr + copied_size), data_size);

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

    switch (target)
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
    memset(uart_rec_buff, 0, sizeof(uart_rec_buff));

    /*
     * 重新开启UART空闲中断接收。
     */
    HAL_UARTEx_ReceiveToIdle_IT(&huart1, uart_rec_buff, BOOT_FRAME_MAX_SIZE);
}

/**
 * @brief 构造并发送一张ACK/NACK应答帧。
 *
 * @param response_cmd 应答类型，只能是CMD_ACK或CMD_NACK。
 * @param request_cmd  本次应答对应的原始请求命令。
 * @param result       请求处理结果。
 * @param value        附加信息，例如包序号或期望包序号。
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
 */
static void Boot_HandleStartFrame(uint8_t *frame, uint16_t frame_len)
{
    Boot_StartInfoTypeDef start_info;
    HAL_StatusTypeDef status;
    uint32_t target_region_size;
    uint32_t run_region_size;

    /*
     * START目标转换成Flag模块使用的槽位。
     */

    Boot_SlotTypeDef target_slot;

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
    printf("TARGET:%u\r\n", (unsigned int)start_info.target);
    printf("IMAGE_SIZE:%lu\r\n", (unsigned long)start_info.image_size);
    printf("IMAGE_CRC:0x%04X\r\n", (unsigned int)start_info.image_crc);

    /*
     * 当前已经处于升级接收状态，
     * 说明之前已经成功执行过一张START帧。
     */
    if (protocol_update_state == BOOT_RECEIVE)
    {

        /*
         * 判断当前START是否是上一张START的重复发送。
         *
         * 必须同时满足：
         *
         * 1. 还没有接收任何BIN数据；
         * 2. 还在等待第0包；
         * 3. 升级目标相同；
         * 4. 固件大小相同；
         * 5. 固件CRC相同。
         *
         * 满足这些条件，说明很可能是START ACK丢失，
         * Qt重新发送了完全相同的START帧。
         */
        if ((protocol_received_size == 0U) &&
            (protocol_expected_sequence == 0U) &&
            (start_info.target == protocol_update_info.target) &&
            (start_info.image_size == protocol_update_info.image_size) &&
            (start_info.image_crc == protocol_update_info.image_crc))
        {
            printf("START_DUPLICATE\r\n");

            /*
             * 目标区已经擦除完成，
             * 不能再次调用Boot_StartUpdate()。
             *
             * 这里只重新发送上一张START的ACK。
             */
            (void)Boot_SendResponse(
                CMD_ACK,
                CMD_START_UPDATE,
                BOOT_RESULT_OK,
                (uint32_t)protocol_update_info.target);

            return;
        }

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
        target_slot = BOOT_SLOT_A;
    }
    else
    {
        target_region_size = APP_B_END_ADDR - APP_B_ADDR + 1U;
        target_slot = BOOT_SLOT_B;
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
     * 在真正擦除A/B区之前，
     * 先把对应槽位标记为无效。
     *
     * 如果后续擦除、接收或写入过程中断电，
     * 下次启动也不会使用这份残缺固件。
     */
    status = Boot_FlagInvalidateImage(target_slot);

    if (status != HAL_OK)
    {
        protocol_update_state = BOOT_ERROR;

        printf("FLAG_INVALIDATE_ERROR\r\n");

        /*
         * Flag失效状态没有保存成功，
         * 此时不能继续擦除目标固件区。
         */
        (void)Boot_SendResponse(
            CMD_NACK,
            CMD_START_UPDATE,
            BOOT_RESULT_FLASH_ERROR,
            (uint32_t)status);

        return;
    }

    printf("FLAG_INVALIDATE_OK\r\n");

    /*
     * Flag已经记录目标槽位无效，
     * 现在才可以安全擦除目标区。
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
 */
static void Boot_HandleDataFrame(uint8_t *frame, uint16_t frame_len)
{
    Boot_DataInfoTypeDef data_info;
    HAL_StatusTypeDef status;

    uint32_t remaining_size;

    if (protocol_update_state != BOOT_RECEIVE)
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
        if ((protocol_expected_sequence > 0U) &&
            (data_info.sequence == (protocol_expected_sequence - 1U)))
        {
            printf("DATA_DUPLICATE\r\n");
            printf("SEQ:%lu\r\n", (unsigned long)data_info.sequence);

            (void)Boot_SendResponse(
                CMD_ACK,
                CMD_DATA_PACKET,
                BOOT_RESULT_OK,
                data_info.sequence);

            return;
        }

        /*
         * 既不是当前需要的包，
         * 也不是刚刚成功写入的上一包，
         * 说明序号确实发生错误。
         */
        printf("SEQUENCE_ERROR\r\n");
        printf("EXPECTED:%lu\r\n", (unsigned long)protocol_expected_sequence);
        printf("RECEIVED:%lu\r\n", (unsigned long)data_info.sequence);

        /*
         * value返回STM32当前期望的包序号。
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
    status = Boot_WriteCache(data_info.data, data_info.data_len);
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
    printf("SEQ:%lu\r\n", (unsigned long)data_info.sequence);
    printf("DATA_LEN:%u\r\n", (unsigned int)data_info.data_len);
    printf("RECEIVED_SIZE:%lu\r\n", (unsigned long)protocol_received_size);

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
     * 保存经过转换后的Flag槽位。
     */
    Boot_SlotTypeDef ready_slot;

    /*
     * 先检查END帧本身是否正确，
     * 并从RESERVE字段解析Qt声明的DATA总包数。
     *
     * 这里必须先解析，再判断升级状态，
     * 因为识别重复END需要使用packet_count。
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
    printf("PACKET_COUNT:%lu\r\n", (unsigned long)packet_count);

    /*
     * 如果当前已经是BOOT_READY，
     * 说明之前的一张END已经完成了：
     *
     * 1. DATA总包数检查；
     * 2. 固件总大小检查；
     * 3. Flash缓存刷新；
     * 4. 整个固件CRC检查。
     *
     * 此时再次收到相同END，
     * 很可能是上一张END ACK丢失。
     */
    if (protocol_update_state == BOOT_READY)
    {
        /*
         * 判断这是不是上一张成功END的重复发送。
         *
         * packet_count必须等于STM32实际接收的包数，
         * received_size也必须等于START声明的固件大小。
         */
        if ((packet_count == protocol_expected_sequence) &&
            (protocol_received_size == protocol_update_info.image_size))
        {
            printf("END_DUPLICATE\r\n");

            /*
             * 固件已经校验成功，
             * 不需要再次刷新缓存或重新计算CRC。
             *
             * 只重新发送上一张END的ACK。
             */
            (void)Boot_SendResponse(
                CMD_ACK,
                CMD_END_UPDATE,
                BOOT_RESULT_OK,
                protocol_expected_sequence);

            return;
        }

        /*
         * 当前虽然处于BOOT_READY，
         * 但收到的END包数与上次升级结果不一致。
         */
        printf("END_DUPLICATE_ERROR\r\n");

        (void)Boot_SendResponse(
            CMD_NACK,
            CMD_END_UPDATE,
            BOOT_RESULT_PACKET_COUNT_ERROR,
            protocol_expected_sequence);

        return;
    }

    /*
     * 如果不是重复END，
     * 那么普通END只能在BOOT_RECEIVE状态下处理。
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
     * protocol_expected_sequence从0开始，
     * 每成功接收一包就加1。
     *
     * 因此它当前也等于已经成功接收的总包数。
     */
    if (packet_count != protocol_expected_sequence)
    {
        printf("PACKET_COUNT_ERROR\r\n");
        printf("EXPECTED_PACKETS:%lu\r\n", (unsigned long)protocol_expected_sequence);
        printf("RECEIVED_PACKETS:%lu\r\n", (unsigned long)packet_count);

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
        printf("EXPECTED_SIZE:%lu\r\n", (unsigned long)protocol_update_info.image_size);
        printf("RECEIVED_SIZE:%lu\r\n", (unsigned long)protocol_received_size);

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
        ready_slot = BOOT_SLOT_A;
    }
    else if (protocol_update_info.target == UPDATE_APP_B)
    {
        image_addr = APP_B_ADDR;
        ready_slot = BOOT_SLOT_B;
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
     */
    calculated_image_crc = Boot_CRC16_Modbus((const uint8_t *)image_addr, protocol_update_info.image_size);

    printf("EXPECTED_IMAGE_CRC:0x%04X\r\n", (unsigned int)protocol_update_info.image_crc);
    printf("CALCULATED_IMAGE_CRC:0x%04X\r\n", (unsigned int)calculated_image_crc);

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

    printf("IMAGE_CRC_OK\r\n");

    /*将这份有效固件的信息保存到Flag区。*/
    status = Boot_FlagSetPendingImage(
        ready_slot,
        protocol_update_info.image_size,
        protocol_update_info.image_crc);

    if (status != HAL_OK)
    {
        protocol_update_state = BOOT_ERROR;
        printf("FLAG_UPDATE_ERROR\r\n");
        (void)Boot_SendResponse(
            CMD_NACK,
            CMD_END_UPDATE,
            BOOT_RESULT_FLASH_ERROR,
            (uint32_t)status);
        return;
    }
    /*整个升级文件校验成功。*/
    protocol_update_state = BOOT_READY;
    printf("FLAG_UPDATE_OK\r\n");
    printf("UPDATE_READY\r\n");

    /*
     * 固件大小、包数和整个BIN CRC和flag全部正确。
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
    printf("CMD:0x%04X\r\n", (unsigned int)cmd);
    printf("LEN:%u\r\n", (unsigned int)frame_len);

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
        printf("UNKNOWN_CMD:0x%04X\r\n", (unsigned int)cmd);
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

    /*
     * 当前还没有执行过启动决定，
     * 并且协议仍处于空闲状态时，
     * 才检查是否等待超时。
     */
    if ((boot_start_decision_done == 0U) &&
        (protocol_update_state == BOOT_IDLE))
    {
        /*
        * 当前时间减去开始时间，
        * 就是已经等待的毫秒数。
        */
        if ((HAL_GetTick() - boot_wait_start_tick) >= BOOT_WAIT_TIMEOUT_MS)
        {
            /*
            * 先置1，保证下面的操作只执行一次。
            */
            boot_start_decision_done = 1U;
            printf("BOOT_WAIT_TIMEOUT\r\n");

           /*
            * 超时后准备运行区：
            *
            * 有pending时完成安装；
            * 没有pending时检查现有运行区。
            */
            if (Boot_PrepareRunImage() != 0U)
            {
                printf("BOOT_AUTO_PREPARE_OK\r\n");

                /*
                * 给串口留一点时间，
                * 确保前面的日志发送完成。
                */
                HAL_Delay(100U);

                /*
                * 跳转到运行区APP。
                */
                Boot_JumpToApp();

                /*
                * 正常跳转后不会返回。
                *
                * 如果执行到这里，
                * 说明向量表检查失败或者跳转异常。
                */
                printf("BOOT_AUTO_JUMP_FAILED\r\n");
            }
            else
            {
                /*
                * 安装失败、Flag异常或运行区CRC错误，
                * 都留在Bootloader等待重新升级。
                */
                printf("BOOT_AUTO_PREPARE_FAILED\r\n");
            }
        }
    }
}

void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
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
        memset(uart_rec_buff, 0, sizeof(uart_rec_buff));

        HAL_UARTEx_ReceiveToIdle_IT(&huart1, uart_rec_buff, PACKET_DATA_SIZE);
    }
}

HAL_StatusTypeDef Boot_InstallPendingImage(void)
{
    const Boot_FlagInfoTypeDef *flag_info;
    const Boot_ImageInfoTypeDef *image_info;

    HAL_StatusTypeDef status;

    uint32_t source_addr;
    uint32_t image_size;

    uint16_t expected_crc;
    uint16_t calculated_crc;

    /*
     * 获取Boot_FlagInit()已经读取到RAM中的
     * 当前Flag信息。
     */
    flag_info = Boot_FlagGetInfo();

    if (flag_info == NULL)
    {
        return HAL_ERROR;
    }

    /*
     * 只有PENDING或者INSTALLING状态
     * 才表示存在需要执行或重新执行的安装任务。
     */
    if ((flag_info->install_state != BOOT_INSTALL_PENDING) &&
        (flag_info->install_state != BOOT_INSTALLING))
    {
        return HAL_ERROR;
    }

    /*
     * 根据pending_slot选择源固件。
     */
    if (flag_info->pending_slot == BOOT_SLOT_A)
    {
        source_addr = APP_A_ADDR;
        image_info = &flag_info->app_a;
    }
    else if (flag_info->pending_slot == BOOT_SLOT_B)
    {
        source_addr = APP_B_ADDR;
        image_info = &flag_info->app_b;
    }
    else
    {
        return HAL_ERROR;
    }

    /*
     * 待安装固件必须已经通过END校验。
     */
    if ((image_info->valid_mark != BOOT_IMAGE_VALID_MARK) ||
        (image_info->image_size == 0U))
    {
        return HAL_ERROR;
    }

    /*
     * 在修改Flag状态之前，
     * 先保存本次安装需要使用的大小和CRC。
     */
    image_size = image_info->image_size;
    expected_crc = image_info->image_crc;

    printf("INSTALL_SOURCE:%lu\r\n", (unsigned long)flag_info->pending_slot);
    printf("INSTALL_SIZE:%lu\r\n", (unsigned long)image_size);
    printf("INSTALL_EXPECTED_CRC:0x%04X\r\n", (unsigned int)expected_crc);

    /*
     * 将Flag状态修改为INSTALLING。
     *
     * 如果当前本来就是INSTALLING，
     * 说明上一次搬运可能被复位打断，
     * 函数会允许重新搬运。
     */
    status = Boot_FlagBeginInstall();

    if (status != HAL_OK)
    {
        printf("INSTALL_BEGIN_ERROR\r\n");
        return status;
    }

    printf("INSTALL_BEGIN_OK\r\n");

    /*
     * 擦除运行区，
     * 然后把A/B槽位中的固件复制到运行区。
     */
    status = Boot_CopyToRun(source_addr, image_size);

    if (status != HAL_OK)
    {
        printf("INSTALL_COPY_ERROR\r\n");

        /*
         * 记录本次安装失败。
         */
        (void)Boot_FlagSetInstallError();

        return status;
    }

    printf("INSTALL_COPY_OK\r\n");

    /*
     * 根据运行区中的真实数据，
     * 重新计算整个APP的CRC16。
     */
    calculated_crc = Boot_CRC16_Modbus((const uint8_t *)APP_RUN_ADDR, image_size);

    printf("INSTALL_RUN_CRC:0x%04X\r\n", (unsigned int)calculated_crc);

    /*
     * 运行区CRC必须与A/B备份固件CRC一致。
     */
    if (calculated_crc != expected_crc)
    {
        printf("INSTALL_CRC_ERROR\r\n");
        (void)Boot_FlagSetInstallError();
        return HAL_ERROR;
    }
    printf("INSTALL_CRC_OK\r\n");
    /*
     * 到这里说明：
     *
     * 1. A/B固件有效；
     * 2. 搬运函数执行成功；
     * 3. 运行区整个APP的CRC正确。
     *
     * 现在才允许更新active_slot，
     * 并将安装状态改为IDLE。
     */
    status = Boot_FlagFinishInstall();

    if (status != HAL_OK)
    {
        printf("INSTALL_FINISH_ERROR\r\n");
        return status;
    }

    printf("INSTALL_FINISH_OK\r\n");

    return HAL_OK;
}

uint8_t Boot_RunImageValid(void)
{
    const Boot_FlagInfoTypeDef *flag_info;
    const Boot_ImageInfoTypeDef *image_info;

    uint32_t run_region_size;
    uint16_t calculated_crc;

    /*
     * 获取Boot_FlagInit()加载到RAM中的Flag。
     */
    flag_info = Boot_FlagGetInfo();

    if (flag_info == NULL)
    {
        return 0U;
    }

    /*
     * 根据active_slot找到运行区对应的源固件信息。
     *
     * active=A：
     * 运行区应该与app_a记录一致。
     *
     * active=B：
     * 运行区应该与app_b记录一致。
     */
    if (flag_info->active_slot == BOOT_SLOT_A)
    {
        image_info = &flag_info->app_a;
    }
    else if (flag_info->active_slot == BOOT_SLOT_B)
    {
        image_info = &flag_info->app_b;
    }
    else
    {
        printf("RUN_ACTIVE_SLOT_ERROR\r\n");
        return 0U;
    }

    /*
     * active槽位必须保存着有效固件信息。
     */
    if (image_info->valid_mark != BOOT_IMAGE_VALID_MARK)
    {
        printf("RUN_IMAGE_MARK_ERROR\r\n");
        return 0U;
    }

    /*
     * 检查固件大小是否合法。
     */
    run_region_size = APP_RUN_END_ADDR - APP_RUN_ADDR + 1U;

    if ((image_info->image_size == 0U) ||
        (image_info->image_size > run_region_size))
    {
        printf("RUN_IMAGE_SIZE_ERROR\r\n");
        return 0U;
    }

    printf("RUN_SOURCE:%lu\r\n", (unsigned long)flag_info->active_slot);
    printf("RUN_SIZE:%lu\r\n", (unsigned long)image_info->image_size);
    printf("RUN_EXPECTED_CRC:0x%04X\r\n", (unsigned int)image_info->image_crc);

    /*
     * 根据运行区真实内容计算整个APP的CRC。
     */
    calculated_crc = Boot_CRC16_Modbus((const uint8_t *)APP_RUN_ADDR, image_info->image_size);

    printf("RUN_CALCULATED_CRC:0x%04X\r\n", (unsigned int)calculated_crc);

    /*
     * 运行区CRC必须与active槽位保存的CRC一致。
     */
    if (calculated_crc != image_info->image_crc)
    {
        printf("RUN_IMAGE_CRC_ERROR\r\n");
        return 0U;
    }

    printf("RUN_IMAGE_OK\r\n");
    return 1U;
}

uint8_t Boot_PrepareRunImage(void)
{
    const Boot_FlagInfoTypeDef *flag_info;
    HAL_StatusTypeDef status;

    /*
     * 获取当前Flag状态。
     */
    flag_info = Boot_FlagGetInfo();

    if (flag_info == NULL)
    {
        printf("BOOT_FLAG_POINTER_ERROR\r\n");
        return 0U;
    }

    printf("BOOT_ACTIVE_SLOT:%lu\r\n", (unsigned long)flag_info->active_slot);
    printf("BOOT_PENDING_SLOT:%lu\r\n", (unsigned long)flag_info->pending_slot);
    printf("BOOT_INSTALL_STATE:%lu\r\n", (unsigned long)flag_info->install_state);

    /*
     * PENDING：
     * 存在一份尚未安装的新固件。
     *
     * INSTALLING：
     * 上一次安装可能被复位打断。
     *
     * 两种状态都需要执行或重新执行搬运。
     */
    if ((flag_info->install_state == BOOT_INSTALL_PENDING) ||
        (flag_info->install_state == BOOT_INSTALLING))
    {
        printf("BOOT_INSTALL_REQUIRED\r\n");

        status = Boot_InstallPendingImage();

        if (status != HAL_OK)
        {
            printf("BOOT_INSTALL_FAILED\r\n");
            return 0U;
        }

        printf("BOOT_INSTALL_OK\r\n");
    }

    /*
     * IDLE表示没有待安装任务，
     * 可以检查当前运行区。
     */
    else if (flag_info->install_state == BOOT_INSTALL_IDLE)
    {
        printf("BOOT_NO_PENDING_IMAGE\r\n");
    }

    /*
     * ERROR或者其他非法状态下，
     * 不允许直接运行APP。
     */
    else
    {
        printf("BOOT_INSTALL_STATE_ERROR\r\n");
        return 0U;
    }

    /*
     * 无论是刚刚完成安装，
     * 还是原本就没有待安装任务，
     * 最后都要检查运行区CRC。
     */
    if (Boot_RunImageValid() == 0U)
    {
        printf("BOOT_RUN_IMAGE_INVALID\r\n");
        return 0U;
    }

    printf("BOOT_RUN_IMAGE_READY\r\n");

    return 1U;
}
