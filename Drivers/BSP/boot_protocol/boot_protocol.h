#ifndef __BOOT_PROTOCOL_H
#define __BOOT_PROTOCOL_H
#include "boot_cache.h"

#define BOOT_FRAME_RESERVE_SIZE   4U
#define BOOT_FRAME_CRC_SIZE       2U

typedef enum
{
    CMD_GET_VERSION = 0x0001,
    CMD_START_UPDATE = 0x0002,
    CMD_DATA_PACKET = 0x0003,
    CMD_END_UPDATE = 0x0004,
    CMD_CHECK_UPDATE = 0x0005,
    CMD_JUMP_APP = 0x0006
}Boot_CmdTypeDef;

typedef enum
{
    BOOT_IDLE = 0,
    BOOT_ERASE,
    BOOT_RECEIVE,
    BOOT_CHECK,
    BOOT_READY,
    BOOT_ERROR
}Boot_StateTypeDef;

typedef enum
{
    UPDATE_NONE = 0,
    UPDATE_APP_A,
    UPDATE_APP_B
}Update_TargetTypeDef;


uint16_t Boot_ParseCmd(uint8_t *frame);
uint16_t Boot_ParseLength(uint8_t *frame);
uint16_t Boot_ParseCRC(uint8_t *frame, uint16_t len);

#endif
