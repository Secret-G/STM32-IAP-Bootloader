#include "app_update.h"
#include "boot_flash.h"

typedef struct
{
    /* 保存一次完整升级会话的状态和接收进度。 */
    AppUpdate_StateTypeDef state;
    Boot_StartInfoTypeDef image;
    uint32_t received_size;
    uint32_t expected_sequence;
} AppUpdate_ContextTypeDef;

static AppUpdate_ContextTypeDef app_update_context;

static void AppUpdate_ResetContext(void)
{
    app_update_context.state = APP_UPDATE_IDLE;
    app_update_context.image.target = UPDATE_NONE;
    app_update_context.image.image_size = 0U;
    app_update_context.image.image_crc = 0U;
    app_update_context.image.image_version = 0;
    app_update_context.received_size = 0U;
    app_update_context.expected_sequence = 0U;
}

static uint8_t AppUpdate_StartInfoMatches(const Boot_StartInfoTypeDef *start_info)
{
    if ((start_info->target == app_update_context.image.target) &&
        (start_info->image_size == app_update_context.image.image_size) &&
        (start_info->image_crc == app_update_context.image.image_crc) &&
        (start_info->image_version == app_update_context.image.image_version))
    {
        return 1U;
    }

    return 0U;
}

void AppUpdate_Init(void)
{
    AppUpdate_ResetContext();
}

void AppUpdate_Abort(void)
{
    /* 当前阶段没有写Flash，断开连接时只需丢弃本次会话状态。 */
    AppUpdate_ResetContext();
}

Boot_ResultTypeDef AppUpdate_HandleStart(const Boot_StartInfoTypeDef *start_info,uint32_t *response_value)
{
    uint32_t staging_capacity;
    uint32_t run_capacity;

    if ((start_info == NULL) || (response_value == NULL))
    {
        return BOOT_RESULT_FRAME_ERROR;
    }

    *response_value = (uint32_t)app_update_context.state;

    if (app_update_context.state == APP_UPDATE_RECEIVING)
    {
        /* START的ACK丢失时，只重新应答，不重复初始化升级。 */
        if ((app_update_context.received_size == 0U) &&
            (app_update_context.expected_sequence == 0U) &&
            (AppUpdate_StartInfoMatches(start_info) != 0U))
        {
            *response_value = (uint32_t)app_update_context.image.target;
            return BOOT_RESULT_OK;
        }

        return BOOT_RESULT_STATE_ERROR;
    }

    if (app_update_context.state != APP_UPDATE_IDLE)
    {
        return BOOT_RESULT_STATE_ERROR;
    }

    staging_capacity = APP_A_END_ADDR - APP_A_ADDR + 1U;
    run_capacity = APP_RUN_END_ADDR - APP_RUN_ADDR + 1U;

    if ((start_info->image_size > staging_capacity) ||
        (start_info->image_size > run_capacity))
    {
        *response_value = start_info->image_size;
        return BOOT_RESULT_IMAGE_TOO_LARGE;
    }

    /* 保存START信息并进入DATA接收状态。 */
    app_update_context.image = *start_info;
    app_update_context.received_size = 0U;
    app_update_context.expected_sequence = 0U;
    app_update_context.state = APP_UPDATE_RECEIVING;

    *response_value = (uint32_t)app_update_context.image.target;
    return BOOT_RESULT_OK;
}

Boot_ResultTypeDef AppUpdate_HandleData(const Boot_DataInfoTypeDef *data_info, uint32_t *response_value)
{
    uint32_t remaining_size;

    if ((data_info == NULL) || (response_value == NULL))
    {
        return BOOT_RESULT_FRAME_ERROR;
    }

    *response_value = (uint32_t)app_update_context.state;

    if (app_update_context.state != APP_UPDATE_RECEIVING)
    {
        return BOOT_RESULT_STATE_ERROR;
    }

    if (data_info->sequence != app_update_context.expected_sequence)
    {
        /* 上一包ACK丢失时，允许上位机重发上一包。 */
        if ((app_update_context.expected_sequence > 0U) &&
        (data_info->sequence == (app_update_context.expected_sequence - 1U)))
        {
            *response_value = data_info->sequence;
            return BOOT_RESULT_OK;
        }

        *response_value = app_update_context.expected_sequence;
        return BOOT_RESULT_SEQUENCE_ERROR;
    }

    if (app_update_context.received_size > app_update_context.image.image_size)
    {
        app_update_context.state = APP_UPDATE_ERROR;
        *response_value = app_update_context.received_size;
        return BOOT_RESULT_IMAGE_SIZE_ERROR;
    }

    remaining_size = app_update_context.image.image_size - app_update_context.received_size;

    if ((uint32_t)data_info->data_len > remaining_size)
    {
        *response_value = remaining_size;
        return BOOT_RESULT_DATA_TOO_LARGE;
    }

    /* 下一阶段将在这里写Flash；写成功后才能更新下面两个计数。 */
    app_update_context.received_size += data_info->data_len;
    app_update_context.expected_sequence++;

    *response_value = data_info->sequence;
    return BOOT_RESULT_OK;
}

Boot_ResultTypeDef AppUpdate_HandleEnd(
    uint32_t packet_count,
    uint32_t *response_value)
{
    if (response_value == NULL)
    {
        return BOOT_RESULT_FRAME_ERROR;
    }

    *response_value = (uint32_t)app_update_context.state;

    if (app_update_context.state == APP_UPDATE_READY)
    {
        /* END的ACK丢失时，只重新返回上一次成功应答。 */
        if ((packet_count == app_update_context.expected_sequence) &&
            (app_update_context.received_size ==
             app_update_context.image.image_size))
        {
            *response_value = app_update_context.expected_sequence;
            return BOOT_RESULT_OK;
        }

        *response_value = app_update_context.expected_sequence;
        return BOOT_RESULT_PACKET_COUNT_ERROR;
    }

    if (app_update_context.state != APP_UPDATE_RECEIVING)
    {
        return BOOT_RESULT_STATE_ERROR;
    }

    if (packet_count != app_update_context.expected_sequence)
    {
        *response_value = app_update_context.expected_sequence;
        return BOOT_RESULT_PACKET_COUNT_ERROR;
    }

    if (app_update_context.received_size !=
        app_update_context.image.image_size)
    {
        *response_value = app_update_context.received_size;
        return BOOT_RESULT_IMAGE_SIZE_ERROR;
    }

    /* 所有包数和字节数正确，本次升级会话进入准备完成状态。 */
    app_update_context.state = APP_UPDATE_READY;
    *response_value = app_update_context.expected_sequence;
    return BOOT_RESULT_OK;
}

AppUpdate_StateTypeDef AppUpdate_GetState(void)
{
    return app_update_context.state;
}
