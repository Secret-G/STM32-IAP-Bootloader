#include "int_bootloader.h"

uint8_t uart_rec_buff[PACKET_DATA_SIZE] = {0};

uint16_t uart_rec_len = 0;

uint8_t sector_erase_flag[FLASH_SECTOR_NUM] = {0};

uint32_t flash_write_addr = APP_START_ADDR;

uint8_t flash_cache[4] = {0};

uint8_t  flash_cache_len = 0U;


volatile uint8_t uart_rx_ready = 0U;
volatile uint32_t uart_total_len = 0;

HAL_StatusTypeDef Flash_Write(uint32_t addr,
                              uint8_t *data,
                              uint32_t len);

HAL_StatusTypeDef Flash_EraseSector(uint32_t sector);

void bootloader_init(void)
{
    __HAL_UART_CLEAR_IDLEFLAG(&huart1);
    __HAL_UART_CLEAR_OREFLAG(&huart1);

    memset(sector_erase_flag, 0, sizeof(sector_erase_flag));
    memset(flash_cache,0,sizeof(flash_cache));

    flash_write_addr = APP_START_ADDR;
    flash_cache_len = 0U;

    HAL_UARTEx_ReceiveToIdle_IT(&huart1, uart_rec_buff, PACKET_DATA_SIZE);
}



HAL_StatusTypeDef Boot_FlushCache(void)
{
    HAL_StatusTypeDef status;
    uint32_t sector = 0;

    uint8_t temp[4] = {0xFF, 0xFF, 0xFF, 0xFF};

    /*
     *remaining_len为0时，表示缓存中没有数据需要写入Flash 
     */
    if (flash_cache_len == 0U)
    {
        return HAL_OK;
    }

    sector = Flash_GetSector(flash_write_addr);
    if (sector == INVALID_SECTOR)
    {
        return HAL_ERROR;
    }

    if (sector_erase_flag[sector] == 0U)
    {
        status = Flash_EraseSector(sector);
        if (status != HAL_OK)
        {
            return status;
        }

        sector_erase_flag[sector] = 1U;
    }
    
    /*
     * 将缓存中剩余的数据写入Flash
     * 由于Flash写入必须4字节对齐，所以需要将剩余数据补齐到4字节
     */
    memcpy(temp, flash_cache, flash_cache_len);

    status = Flash_Write(flash_write_addr, temp, 4U);
    if (status != HAL_OK)
    {
        return status;
    }

    flash_write_addr += 4U;

    memset(flash_cache, 0, sizeof(flash_cache));

    flash_cache_len = 0U;

    return HAL_OK;

}

uint32_t Flash_GetSector(uint32_t addr)
{
    if (addr < FLASH_SECTOR_4_ADDR)
    {
        return INVALID_SECTOR;
    }

    if (addr < FLASH_SECTOR_5_ADDR)
    {
        return FLASH_SECTOR_4;
    }
    else if (addr < FLASH_SECTOR_6_ADDR)
    {
        return FLASH_SECTOR_5;
    }
    else if (addr < FLASH_SECTOR_7_ADDR)
    {
        return FLASH_SECTOR_6;
    }
    else if (addr < FLASH_SECTOR_8_ADDR)
    {
        return FLASH_SECTOR_7;
    }
    else if (addr < FLASH_SECTOR_9_ADDR)
    {
        return FLASH_SECTOR_8;
    }
    else if (addr < FLASH_SECTOR_10_ADDR)
    {
        return FLASH_SECTOR_9;
    }
    else if (addr < FLASH_SECTOR_11_ADDR)
    {
        return FLASH_SECTOR_10;
    }
    else if (addr <= FLASH_END_ADDR)
    {
        return FLASH_SECTOR_11;
    }

    return INVALID_SECTOR;
}

HAL_StatusTypeDef Flash_EraseSector(uint32_t sector)
{
    FLASH_EraseInitTypeDef erase_init = {0};
    HAL_StatusTypeDef status;
    uint32_t sector_error;

    /* 禁止擦除Bootloader所在的Sector 0～3 */
    if ((sector < FLASH_SECTOR_4) ||
        (sector > FLASH_SECTOR_11))
    {
        return HAL_ERROR;
    }

    status = HAL_FLASH_Unlock();
    if (status != HAL_OK)
    {
        return status;
    }

    erase_init.TypeErase = FLASH_TYPEERASE_SECTORS;
    erase_init.Sector = sector;
    erase_init.NbSectors = 1U;
    erase_init.VoltageRange = FLASH_VOLTAGE_RANGE_3;

    status = HAL_FLASHEx_Erase(&erase_init, &sector_error);

    HAL_FLASH_Lock();

    return status;
}

HAL_StatusTypeDef Flash_Write(uint32_t addr,
                              uint8_t *data,
                              uint32_t len)
{
    uint32_t i;
    uint32_t temp;

    HAL_StatusTypeDef status;

    if((data == NULL) ||
       (len == 0U) ||
       ((addr & 0x03U)!=0U) ||
       ((len & 0x03U)!=0U))
    {
        return HAL_ERROR;
    }


    if((addr < APP_START_ADDR) ||
       (addr > APP_END_ADDR) ||
       (len > (APP_END_ADDR - addr + 1U)))
    {
        return HAL_ERROR;
    }


    status = HAL_FLASH_Unlock();

    if(status != HAL_OK)
    {
        return status;
    }


    for(i=0;i<len;i+=4U)
    {

        temp = ((uint32_t)data[i])
             | ((uint32_t)data[i+1U]<<8)
             | ((uint32_t)data[i+2U]<<16)
             | ((uint32_t)data[i+3U]<<24);


        status = HAL_FLASH_Program(
                    FLASH_TYPEPROGRAM_WORD,
                    addr+i,
                    temp);


        if(status != HAL_OK)
        {
            break;
        }


        if(*(volatile uint32_t *)(addr+i)!=temp)
        {
            status = HAL_ERROR;
            break;
        }

    }


    HAL_FLASH_Lock();


    return status;
}



HAL_StatusTypeDef Boot_WriteCache(uint8_t *data,uint32_t len)
{
    uint32_t i = 0;
    uint32_t sector = 0;
    HAL_StatusTypeDef status;

    for ( i = 0; i < len; i++)
    {
        /*
         * 先将数据写入缓存，等缓存满4字节后再写入Flash
         * 这样可以保证Flash写入地址始终4字节对齐
         */
        flash_cache[flash_cache_len] = data[i];

        flash_cache_len++;

        /*
         * 缓存满4字节，写入Flash
         */
        if (flash_cache_len == 4U)
        {

            sector = Flash_GetSector(flash_write_addr);

            if (sector == INVALID_SECTOR)
            {
                return HAL_ERROR;
            }

            if(sector_erase_flag[sector] == 0U)
            {
                status = Flash_EraseSector(sector);
                if (status != HAL_OK)
                {
                    return status;
                }

                sector_erase_flag[sector] = 1U;
            }

            status = Flash_Write(flash_write_addr, flash_cache, 4U);
            if (status != HAL_OK)
            {
                return status;
            }

            flash_write_addr += 4U;

            flash_cache_len = 0U;

            memset(flash_cache, 0, sizeof(flash_cache));
        }

    }
    
    return HAL_OK;
}
/**
 * @brief 串口接收回调函数
 *
 * @param huart
 * @param Size
 */
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart,uint16_t Size)
{
    HAL_StatusTypeDef status;

    if (huart->Instance == USART1)
    {
        status = Boot_WriteCache(uart_rec_buff,Size);

/*        if (status == HAL_OK)
        {
            uart_total_len += Size;

            if (Size < PACKET_DATA_SIZE)
            {
                status = Boot_FlushCache();
            }
        }

        if (status == HAL_OK)
        {
            uart_rx_ready = 1U;
        }
        else
        {
            uart_rx_ready = 2U;
        }*/

        memset(uart_rec_buff,0,PACKET_DATA_SIZE);

        HAL_UARTEx_ReceiveToIdle_IT(&huart1,uart_rec_buff,PACKET_DATA_SIZE);
    }
}
