#include "boot_cache.h"
#include <string.h>

/*
 * 这些变量只属于boot_cache模块，
 * int_bootloader不能再直接访问和修改。
 */
static uint8_t flash_cache[4] = {0};
static uint8_t flash_cache_len = 0U;
static uint8_t sector_erase_flag[FLASH_SECTOR_NUM] = {0};
static uint32_t flash_write_addr = APP_START_ADDR;

static HAL_StatusTypeDef BootCache_PrepareSector(void)
{
    uint32_t sector;
    HAL_StatusTypeDef status;

    sector = Flash_GetSector(flash_write_addr);
    if (sector == INVALID_SECTOR)
    {
        return HAL_ERROR;
    }

    if (sector_erase_flag[sector] != 0U)
    {
        return HAL_OK;
    }

    status = Flash_EraseSector(sector);
    if (status == HAL_OK)
    {
        sector_erase_flag[sector] = 1U;
    }

    return status;
}

static HAL_StatusTypeDef BootCache_WriteWord(uint8_t data[4])
{
    HAL_StatusTypeDef status;

    status = BootCache_PrepareSector();
    if (status != HAL_OK)
    {
        return status;
    }

    status = Flash_Write(flash_write_addr, data, 4U);
    if (status == HAL_OK)
    {
        flash_write_addr += 4U;
    }

    return status;
}

void BootCache_Init(void)
{
    memset(flash_cache, 0, sizeof(flash_cache));
    memset(sector_erase_flag, 0, sizeof(sector_erase_flag));

    flash_cache_len = 0U;
    flash_write_addr = APP_START_ADDR;
}

HAL_StatusTypeDef Boot_WriteCache(const uint8_t *data, uint32_t len)
{
    uint32_t i;
    HAL_StatusTypeDef status;

    if ((data == NULL) || (len == 0U))
    {
        return HAL_ERROR;
    }

    /*
     * 如果上一次4字节写入失败，flash_cache_len会保持为4。
     * 此时拒绝继续写，防止缓存越界。
     */
    if (flash_cache_len >= sizeof(flash_cache))
    {
        return HAL_ERROR;
    }

    for (i = 0U; i < len; i++)
    {
        flash_cache[flash_cache_len] = data[i];
        flash_cache_len++;

        if (flash_cache_len == sizeof(flash_cache))
        {
            status = BootCache_WriteWord(flash_cache);
            if (status != HAL_OK)
            {
                return status;
            }

            flash_cache_len = 0U;
            memset(flash_cache, 0, sizeof(flash_cache));
        }
    }

    return HAL_OK;
}

HAL_StatusTypeDef Boot_FlushCache(void)
{
    uint8_t temp[4] = {0xFFU, 0xFFU, 0xFFU, 0xFFU};
    HAL_StatusTypeDef status;

    if (flash_cache_len == 0U)
    {
        return HAL_OK;
    }

    if (flash_cache_len >= sizeof(flash_cache))
    {
        return HAL_ERROR;
    }

    memcpy(temp, flash_cache, flash_cache_len);

    status = BootCache_WriteWord(temp);
    if (status != HAL_OK)
    {
        return status;
    }

    memset(flash_cache, 0, sizeof(flash_cache));
    flash_cache_len = 0U;

    return HAL_OK;
}
