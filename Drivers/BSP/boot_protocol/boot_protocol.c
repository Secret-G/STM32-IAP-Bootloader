#include "boot_protocol.h"



uint16_t Boot_ParseCmd(uint8_t *frame)
{
    uint16_t cmd;
    cmd = ((uint16_t)frame[0]) | ((uint16_t)frame[1] << 8);
    return cmd;
}

uint16_t Boot_ParseLength(uint8_t *frame)
{
    uint16_t len;
    len = ((uint16_t)frame[2]) | ((uint16_t)frame[3] << 8);
    return len;
}

uint16_t Boot_ParseCRC(uint8_t *frame,uint16_t len)
{
    uint16_t crc;
    crc = ((uint16_t)frame[4 + len + 4])| ((uint16_t)frame[5 + len + 4] << 8);
    return crc;
}