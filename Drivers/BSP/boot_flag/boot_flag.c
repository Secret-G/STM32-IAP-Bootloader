#include "boot_flag.h"
#include "boot_crc.h"

/*
 * Bootloader当前使用的标志信息。
 *
 * 启动时从Flash读取到这里，
 * 后续修改A/B状态时也操作这个结构。
 */
static Boot_FlagInfoTypeDef boot_flag_info;


uint32_t Boot_FlagReadMagic(void)
{
    /*
     * Flash可以像普通只读存储器一样直接读取。
     *
     * FLAG_START_ADDR是0x08010000，
     * 这里读取该地址开始的4个字节。
     */
    return *(volatile const uint32_t *)FLAG_START_ADDR;
}

uint8_t Boot_FlagMagicValid(void)
{
    uint32_t stored_magic;

    /*
     * 读取Flash中实际保存的magic。
     */
    stored_magic = Boot_FlagReadMagic();

    /*
     * 与我们规定的固定值比较。
     */
    if (stored_magic == BOOT_FLAG_MAGIC)
    {
        return 1U;
    }

    return 0U;
}

void Boot_FlagSetDefault(Boot_FlagInfoTypeDef *flag_info)
{
    /*
     * 防止调用者传入空地址。
     */
    if (flag_info == NULL)
    {
        return;
    }

    /*设置标志区基本信息*/
    flag_info->magic = BOOT_FLAG_MAGIC;
    flag_info->version = BOOT_FLAG_VERSION;

    /*设置A区固件信息*/
    flag_info->app_a.valid_mark = BOOT_IMAGE_INVALID_MARK;
    flag_info->app_a.image_size = 0U;
    flag_info->app_a.image_crc = 0U;
    flag_info->app_a.reserved = 0U;

    /*设置B区固件信息*/
    flag_info->app_b.valid_mark = BOOT_IMAGE_INVALID_MARK;
    flag_info->app_b.image_size = 0U;
    flag_info->app_b.image_crc = 0U;
    flag_info->app_b.reserved = 0U;

    /*设置当前运行区信息*/
    flag_info->active_slot = BOOT_SLOT_NONE;

    /*设置下一次需要搬运到运行区的目标*/
    flag_info->pending_slot = BOOT_SLOT_NONE;

    /*设置当前安装状态*/
    flag_info->install_state = BOOT_INSTALL_IDLE;

    /*设置保留字段*/
    flag_info->reserved = 0U;

    /*设置整个标志结构的CRC16*/
    flag_info->flag_crc = 0U;

}



uint16_t Boot_FlagCalculateCRC(const Boot_FlagInfoTypeDef *flag_info)
{
     uint32_t crc_length;

    if (flag_info == NULL)
    {
        return 0U;
    }

    /*
     * 完整结构48字节，
     * 最后的flag_crc占2字节。
     *
     * 所以实际参与CRC计算的是前46字节。
     */
    crc_length = sizeof(Boot_FlagInfoTypeDef) - sizeof(flag_info->flag_crc);

        /*
     * CRC函数按字节处理数据，
     * 所以把结构体地址转换成uint8_t指针。
     */
    return Boot_CRC16_Modbus((const uint8_t *)flag_info,crc_length);


}

void Boot_FlagUpdateCRC(Boot_FlagInfoTypeDef *flag_info)
{
    if (flag_info == NULL)
    {
        return;
    }

    /*
     * 计算前46字节的CRC，
     * 保存到最后2字节。
     */
    flag_info->flag_crc = Boot_FlagCalculateCRC(flag_info);
}

uint8_t Boot_FlagCRCValid(const Boot_FlagInfoTypeDef *flag_info)
{
    uint16_t calculated_crc;

    if (flag_info == NULL)
    {
        return 0U;
    }

    /*
     * 根据结构当前内容重新计算CRC。
     */
    calculated_crc = Boot_FlagCalculateCRC(flag_info);

    /*
     * 与结构中原来保存的CRC比较。
     */
    if (calculated_crc == flag_info->flag_crc)
    {
        return 1U;
    }

    return 0U;
}

void Boot_FlagRead(Boot_FlagInfoTypeDef *flag_info)
{
    uint32_t i;

    const volatile uint8_t *flash_data;
    uint8_t *ram_data;

    if (flag_info == NULL)
    {
        return;
    }

    /*
     * flash_data指向Flash标志区起始地址。
     */
    flash_data = (const volatile uint8_t *)FLAG_START_ADDR;

    /*
     * ram_data指向RAM中的标志结构体。
     */
    ram_data = (uint8_t *)flag_info;

    /*
     * 逐字节复制Flash中的数据到RAM结构体中。
     */
    for (i = 0U; i < sizeof(Boot_FlagInfoTypeDef); i++)
    {
        ram_data[i] = flash_data[i];
    }

}

uint8_t Boot_FlagInfoValid(const Boot_FlagInfoTypeDef *flag_info)
{
    if (flag_info == NULL)
    {
        return 0U;
    }

    /*
     * 检查magic是否正确。
     */
    if (flag_info->magic != BOOT_FLAG_MAGIC)
    {
        return 0U;
    }

    /*
     * 检查version是否正确。
     */
    if (flag_info->version != BOOT_FLAG_VERSION)
    {
        return 0U;
    }

    /*
     * 检查整个结构的CRC是否正确。
     */
    if (Boot_FlagCRCValid(flag_info) == 0U)
    {
        return 0U;
    }

    return 1U;
}

HAL_StatusTypeDef Boot_FlagWrite(Boot_FlagInfoTypeDef *flag_info)
{
    HAL_StatusTypeDef status;
    /*
     * 调用者没有提供标志结构。
     */
    if (flag_info == NULL)
    {
        return HAL_ERROR;
    }

    /*
     * 写入Flash之前，根据当前结构内容
     * 重新计算并保存flag_crc。
     */
    Boot_FlagUpdateCRC(flag_info);

    /*
     * Flag区位于Sector 4。
     * 所以写入新标志之前必须先擦除整个扇区。
     */
    status = Flash_EraseSector(FLASH_SECTOR_4);

    if (status != HAL_OK)
    {
        return status;
    }

    /*
     * 从FLAG_START_ADDR开始，
     * 写入完整的48字节标志结构。
     */
    status = Flash_Write(FLAG_START_ADDR,(uint8_t *)flag_info, sizeof(Boot_FlagInfoTypeDef));
    
    return status;
}
HAL_StatusTypeDef Boot_FlagInit(void)
{
    HAL_StatusTypeDef status;
    /*
     * 先把Flash标志区中的48字节
     * 复制到RAM全局结构体中。
     */
    Boot_FlagRead(&boot_flag_info);

    /*
     * Flash中已有合法标志，
     * 不擦除、不重写，直接使用。
     */
    if (Boot_FlagInfoValid(&boot_flag_info) != 0U)
    {
        return HAL_OK;
    }

    /*
     * Flash标志不存在或者已经损坏，
     * 在RAM中生成默认内容。
     */
    Boot_FlagSetDefault(&boot_flag_info);

     /*
     * 将默认内容写入Flash。
     * Boot_FlagWrite内部会计算CRC并擦除Sector 4。
     */
    status = Boot_FlagWrite(&boot_flag_info);

    if (status != HAL_OK)
    {
        return status;
    }
    
    /*
     * 重新从Flash读取，而不是直接相信刚才的RAM数据。
     */
    Boot_FlagRead(&boot_flag_info);

    /*
     * 检查真正写入Flash的内容。
     */
    if (Boot_FlagInfoValid(&boot_flag_info) == 0U)
    {
        return HAL_ERROR;
    }

    return HAL_OK;
}

const Boot_FlagInfoTypeDef* Boot_FlagGetInfo(void)
{

    return &boot_flag_info;
}

HAL_StatusTypeDef Boot_FlagSetPendingImage(Boot_SlotTypeDef slot,uint32_t image_size,uint16_t image_crc)
{
    HAL_StatusTypeDef status;
    Boot_ImageInfoTypeDef *image_info;

    /*
     * 固件大小不能为0。
     */
    if (image_size == 0U)
    {
        return HAL_ERROR;
    }

    if(slot == BOOT_SLOT_A)
    {
        image_info = &boot_flag_info.app_a;
    }
    else if(slot == BOOT_SLOT_B)
    {
        image_info = &boot_flag_info.app_b;
    }
    else
    {
        return HAL_ERROR;
    }

    /*
     * 到达这里说明：
     * 1. 所有DATA都已写入；
     * 2. 文件大小正确；
     * 3. 整个Flash固件CRC正确。
     *
     * 因此可以把对应槽位标记为有效。
     */
    image_info->valid_mark = BOOT_IMAGE_VALID_MARK;
    image_info->image_size = image_size;
    image_info->image_crc = image_crc;
    image_info->reserved = 0U;

    /*记录下一次需要搬运到运行区的槽位。*/
    boot_flag_info.pending_slot = slot;

    /*表示已经存在完整固件，等待搬运。*/
    boot_flag_info.install_state = BOOT_INSTALL_PENDING;

     /*
     * Boot_FlagWrite会：
     * 1. 更新整个标志结构CRC；
     * 2. 擦除Sector 4；
     * 3. 把完整结构写入Flash。
     */
    status = Boot_FlagWrite(&boot_flag_info);

    if (status != HAL_OK)
    {
        Boot_FlagRead(&boot_flag_info);
        return status;
    }
    return HAL_OK;
}

HAL_StatusTypeDef Boot_FlagInvalidateImage(Boot_SlotTypeDef slot)
{
    HAL_StatusTypeDef status;
    Boot_ImageInfoTypeDef *image_info;

    if(slot == BOOT_SLOT_A)
    {
        image_info = &boot_flag_info.app_a;
    }
    else if(slot == BOOT_SLOT_B)
    {
        image_info = &boot_flag_info.app_b;
    }
    else
    {
        return HAL_ERROR;
    }

    /*
     * 将对应槽位标记为无效。
     */
    image_info->valid_mark = BOOT_IMAGE_INVALID_MARK;
    image_info->image_size = 0U;
    image_info->image_crc = 0U;
    image_info->reserved = 0U;

    /*
     * 如果待安装的固件刚好来自当前槽位，
     * 那么这个待安装任务也必须取消。
     *
     * 因为对应槽位接下来将被擦除。
     */
    if (boot_flag_info.pending_slot == slot)
    {
        boot_flag_info.pending_slot = BOOT_SLOT_NONE;
        boot_flag_info.install_state = BOOT_INSTALL_IDLE;
    }

    /*
     * 将新的无效状态保存进Flash标志区。
     */
    status = Boot_FlagWrite(&boot_flag_info);

    if (status != HAL_OK)
    {
        /*
         * 写入失败时重新读取Flash中的状态，
         * 避免RAM与Flash内容不一致。
         */
        Boot_FlagRead(&boot_flag_info);

        return status;
    }

    return HAL_OK;
}

HAL_StatusTypeDef Boot_FlagBeginInstall(void)
{
    HAL_StatusTypeDef status;
    const Boot_ImageInfoTypeDef *image_info;

    /*
     * 根据pending_slot找到待安装固件信息。
     */
    if (boot_flag_info.pending_slot == BOOT_SLOT_A)
    {
        image_info = &boot_flag_info.app_a;
    }
    else if (boot_flag_info.pending_slot == BOOT_SLOT_B)
    {
        image_info = &boot_flag_info.app_b;
    }
    else
    {
        return HAL_ERROR;
    }

    /*
     * 待安装固件必须有效且大小不能为0。
     */
    if ((image_info->valid_mark != BOOT_IMAGE_VALID_MARK) ||
        (image_info->image_size == 0U))
    {
        return HAL_ERROR;
    }

    /*
     * 上一次安装过程中复位了，
     * 保持INSTALLING并允许重新搬运。
     */
    if (boot_flag_info.install_state == BOOT_INSTALLING)
    {
        return HAL_OK;
    }

    /*
     * 正常情况下只能从PENDING开始安装。
     */
    if (boot_flag_info.install_state != BOOT_INSTALL_PENDING)
    {
        return HAL_ERROR;
    }

    boot_flag_info.install_state = BOOT_INSTALLING;

    status = Boot_FlagWrite(&boot_flag_info);

    if (status != HAL_OK)
    {
        Boot_FlagRead(&boot_flag_info);
        return status;
    }

    return HAL_OK;
}

HAL_StatusTypeDef Boot_FlagFinishInstall(void)
{
    HAL_StatusTypeDef status;

    if (boot_flag_info.install_state != BOOT_INSTALLING)
    {
        return HAL_ERROR;
    }

    /*
     * pending_slot必须是合法的A或B。
     */
    if ((boot_flag_info.pending_slot != BOOT_SLOT_A) &&
        (boot_flag_info.pending_slot != BOOT_SLOT_B))
    {
        return HAL_ERROR;
    }

    /*
     * 运行区现在来自pending_slot。
     * 例如pending_slot为B，
     * 搬运成功后active_slot就变成B。
     */
    boot_flag_info.active_slot = boot_flag_info.pending_slot;

    /*待安装任务已经完成。*/
    boot_flag_info.pending_slot = BOOT_SLOT_NONE;
    boot_flag_info.install_state = BOOT_INSTALL_IDLE;

    /*将最终状态保存到Flash。*/
    status = Boot_FlagWrite(&boot_flag_info);

    if (status != HAL_OK)
    {
        Boot_FlagRead(&boot_flag_info);
        return status;
    }

    return HAL_OK;
}

HAL_StatusTypeDef Boot_FlagSetInstallError(void)
{
    HAL_StatusTypeDef status;

    /*
     * 记录安装错误。
     *
     * pending_slot不清除，
     * 方便后续判断是哪一个槽位安装失败。
     */
    boot_flag_info.install_state = BOOT_INSTALL_ERROR;

    status = Boot_FlagWrite(&boot_flag_info);

    if (status != HAL_OK)
    {
        Boot_FlagRead(&boot_flag_info);
        return status;
    }

    return HAL_OK;
}
