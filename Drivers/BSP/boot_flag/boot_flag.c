#include "boot_flag.h"
#include "boot_crc.h"

#include <stddef.h>
#include <string.h>
/*
 * Bootloader当前使用的标志信息。
 *
 * 启动时从Flash读取到这里，
 * 后续修改A/B状态时也操作这个结构。
 */
static Boot_FlagInfoTypeDef boot_flag_info;

/*
 * 当前RAM中的boot_flag_info来自哪个Flash副本。
 *
 * 默认先指向COPY0。
 * Boot_FlagInit()完成双副本检查后，
 * 会把它改成真正选中的副本地址。
 */
static uint32_t boot_flag_current_addr = 0U;

/*
 * 从指定Flash副本地址读取一份完整Flag。
 *
 * 该函数只在boot_flag.c内部使用，
 * 所以使用static，不放到boot_flag.h中。
 */
static void Boot_FlagReadCopy(uint32_t flash_addr, Boot_FlagInfoTypeDef *flag_info);

uint32_t Boot_FlagReadMagic(void)
{
    /*Flash可以像普通只读存储器一样直接读取。*/
    return *(volatile const uint32_t *)FLAG_START_ADDR;
}

uint8_t Boot_FlagMagicValid(void)
{
    uint32_t stored_magic;

    /*读取Flash中实际保存的magic。*/
    stored_magic = Boot_FlagReadMagic();

    /*与我们规定的固定值比较。*/
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
    flag_info->sequence = 0U;

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

    /*CRC覆盖从结构体开头到flag_crc字段之前的所有字节*/
    crc_length = (uint32_t)offsetof(Boot_FlagInfoTypeDef, flag_crc);

    /*
     * CRC函数按字节处理数据，
     * 所以把结构体地址转换成uint8_t指针。
     */
    return Boot_CRC16_Modbus((const uint8_t *)flag_info, crc_length);
}

void Boot_FlagUpdateCRC(Boot_FlagInfoTypeDef *flag_info)
{
    if (flag_info == NULL)
    {
        return;
    }

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

/*从指定Flash副本地址读取一份完整Flag。*/
static void Boot_FlagReadCopy(uint32_t flash_addr, Boot_FlagInfoTypeDef *flag_info)
{
    uint32_t i;

    const volatile uint8_t *flash_data;

    uint8_t *ram_data;

    /*调用者没有提供RAM保存地址*/
    if (flag_info == NULL)
    {
        return;
    }

    /*
     * 只允许读取我们规划的两个Flag副本，
     * 防止错误地址被当成Flag解析。
     */
    if ((flash_addr != FLAG_COPY0_ADDR) && (flash_addr != FLAG_COPY1_ADDR))
    {
        return;
    }

    /*Flash能够像只读内存一样直接访问。*/
    flash_data = (const volatile uint8_t *)flash_addr;

    /*将输出结构体地址转换成字节指针*/
    ram_data = (uint8_t *)flag_info;

    /*将Flash中的完整Flag结构逐字节复制到RAM。*/
    for (i = 0U; i < sizeof(Boot_FlagInfoTypeDef); i++)
    {
        ram_data[i] = flash_data[i];
    }
}

void Boot_FlagRead(Boot_FlagInfoTypeDef *flag_info)
{
    Boot_FlagReadCopy(boot_flag_current_addr, flag_info);
}

uint8_t Boot_FlagInfoValid(const Boot_FlagInfoTypeDef *flag_info)
{
    if (flag_info == NULL){ return 0U;  }

    /*检查magic是否正确。*/
    if (flag_info->magic != BOOT_FLAG_MAGIC){   return 0U;  }

    /*检查version是否正确。*/
    if (flag_info->version != BOOT_FLAG_VERSION){   return 0U;  }

    /*检查整个结构的CRC是否正确。*/
    if (Boot_FlagCRCValid(flag_info) == 0U){   return 0U;  }

    return 1U;
}

HAL_StatusTypeDef Boot_FlagWrite(Boot_FlagInfoTypeDef *flag_info)
{
    HAL_StatusTypeDef status;

    Boot_FlagInfoTypeDef candidate;
    Boot_FlagInfoTypeDef verify_info;

    uint32_t target_addr;
    uint32_t target_sector;

    /*调用者没有提供需要提交的Flag信息。*/
    if (flag_info == NULL){ return HAL_ERROR;   }

    candidate = *flag_info;

    /*每成功提交一次新Flag，sequence都应该比当前状态增加1。*/
    candidate.sequence = flag_info->sequence + 1U;

    /*根据候选副本的全部字段重新计算CRC*/
    Boot_FlagUpdateCRC(&candidate);

    /*
     * 选择当前有效副本的另一侧作为写入目标。
     *
     * 当前是COPY0：
     * 新状态写到COPY1。
     */
    if (boot_flag_current_addr == FLAG_COPY0_ADDR)
    {
        target_addr = FLAG_COPY1_ADDR;
        target_sector = FLASH_SECTOR_11;
    }
    /*当前是COPY1：新状态写到COPY0。*/
    else if (boot_flag_current_addr == FLAG_COPY1_ADDR)
    {
        target_addr = FLAG_COPY0_ADDR;
        target_sector = FLASH_SECTOR_4;
    }

    /*当前还没有有效副本：第一份默认Flag写入COPY0*/
    else if (boot_flag_current_addr == 0U)
    {
        target_addr = FLAG_COPY0_ADDR;
        target_sector = FLASH_SECTOR_4;
    }

    /*current_addr出现非法地址。*/
    else
    {
        return HAL_ERROR;
    }

    /*
     * 只擦除即将写入的新副本。
     *
     * 当前旧副本保持不动，
     * 因此这里掉电也不会丢失旧状态。
     */
    status = Flash_EraseSector(target_sector);
    if (status != HAL_OK)
    {
        return status;
    }

     /*
     * 将完整候选Flag写入目标副本。
     */
    status = Flash_Write(target_addr,(uint8_t *)&candidate,sizeof(Boot_FlagInfoTypeDef));
    if (status != HAL_OK)
    {
        return status;
    }

    /*从Flash目标地址重新读取，不能直接相信Flash_Write()的返回值。*/
    Boot_FlagReadCopy(target_addr,&verify_info);

    /*先检查Magic、Version和CRC*/
    if (Boot_FlagInfoValid(&verify_info) == 0U){    return HAL_ERROR;   }

    /*
     * 再逐字节比较：
     *
     * candidate是准备写入的数据；
     * verify_info是Flash真实回读的数据。
     *
     * 返回0才表示52字节完全相同。
     */
    if (memcmp(&candidate, &verify_info, sizeof(Boot_FlagInfoTypeDef)) != 0)
    {
        return HAL_ERROR;
    }

    /*
     * 到这里才表示事务提交成功：
     *
     * 1. 目标扇区擦除成功；
     * 2. 新Flag写入成功；
     * 3. 回读结构有效；
     * 4. 回读内容完全相同。
     *
     * 现在才允许切换当前有效副本。
     */
    boot_flag_current_addr = target_addr;

    /*
     * 同时更新调用者提供的结构，
     * 让它获得最新sequence和CRC。
     *
     * 当前调用者通常就是&boot_flag_info，
     * 但这里仍然保持接口完整。
     */
    *flag_info = verify_info;

    return HAL_OK;

}

HAL_StatusTypeDef Boot_FlagInit(void)

{
    HAL_StatusTypeDef status;
    Boot_FlagInfoTypeDef copy0_info;
    Boot_FlagInfoTypeDef copy1_info;

    uint8_t copy0_valid;
    uint8_t copy1_valid;

    /*分别读取Sector 4和Sector 11中的Flag副本。*/
    Boot_FlagReadCopy(FLAG_COPY0_ADDR, &copy0_info);
    Boot_FlagReadCopy(FLAG_COPY1_ADDR, &copy1_info);

    copy0_valid = Boot_FlagInfoValid(&copy0_info);
    copy1_valid = Boot_FlagInfoValid(&copy1_info);

    /*两份都有效：选择sequence更大的副本。*/
    if ((copy0_valid != 0U) && (copy1_valid != 0U))
    {
        if (copy1_info.sequence > copy0_info.sequence)
        {
            boot_flag_info = copy1_info;
            boot_flag_current_addr = FLAG_COPY1_ADDR;
        }
        else
        {
            /*COPY0的sequence更大，或者两份sequence相等时，确定性地选择COPY0。*/
            boot_flag_info = copy0_info;
            boot_flag_current_addr = FLAG_COPY0_ADDR;
        }
        return HAL_OK;
    }

    /*只有COPY0有效。*/
    if (copy0_valid != 0U)
    {
        boot_flag_info = copy0_info;
        boot_flag_current_addr = FLAG_COPY0_ADDR;
        return HAL_OK;
    }

    /*只有COPY1有效。*/
    if (copy1_valid != 0U)
    {
        boot_flag_info = copy1_info;
        boot_flag_current_addr = FLAG_COPY1_ADDR;
        return HAL_OK;
    }

    /*
     * 两份副本都无效。
     *
     * 可能是：
     * 1. 芯片第一次运行；
     * 2. Flash仍然是0xFF；
     * 3. 旧版V1 Flag；
     * 4. 两份副本都已损坏。
     */
    Boot_FlagSetDefault(&boot_flag_info);

    /*
     * 当前没有任何有效副本。
     * 保持current_addr为0，Boot_FlagWrite()会把第一份Flag写入COPY0。
     */
    boot_flag_current_addr = 0U;

    status = Boot_FlagWrite(&boot_flag_info);

    if (status != HAL_OK){  return status;  }

    /*从真正写入的COPY0重新读取。*/
    Boot_FlagRead(&boot_flag_info);

    /*不相信RAM原值，检查Flash中的真实内容。*/
    if (Boot_FlagInfoValid(&boot_flag_info) == 0U){ return HAL_ERROR;   }

    return HAL_OK;
}

const Boot_FlagInfoTypeDef *Boot_FlagGetInfo(void)
{

    return &boot_flag_info;
}

HAL_StatusTypeDef Boot_FlagSetPendingImage(Boot_SlotTypeDef slot, uint32_t image_size, uint16_t image_crc)
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

    if (slot == BOOT_SLOT_A)
    {
        image_info = &boot_flag_info.app_a;
    }
    else if (slot == BOOT_SLOT_B)
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

    if (slot == BOOT_SLOT_A)
    {
        image_info = &boot_flag_info.app_a;
    }
    else if (slot == BOOT_SLOT_B)
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
