#include "tcp_server.h"
#include "app_update.h"
#include "boot_protocol.h"

#include "cmsis_os.h"
#include "lwip/api.h"

#include <stdio.h>

#define TCP_SERVER_PORT 5000U

static err_t TcpServer_SendResponse(struct netconn *client,
                                    Boot_CmdTypeDef request_cmd,
                                    Boot_ResultTypeDef result,
                                    uint32_t value)
{
    uint8_t response[BOOT_RESPONSE_FRAME_SIZE];
    uint16_t response_length;
    Boot_CmdTypeDef response_cmd;

    response_cmd = (result == BOOT_RESULT_OK) ? CMD_ACK : CMD_NACK;
    response_length = Boot_BuildResponseFrame(response,
                                              sizeof(response),
                                              response_cmd,
                                              request_cmd,
                                              result,
                                              value);
    if (response_length == 0U)
    {
        return ERR_ARG;
    }

    return netconn_write(client, response, response_length, NETCONN_COPY);
}

static err_t TcpServer_ProcessFrame(struct netconn *client,
                                    uint8_t *frame,
                                    uint16_t frame_length)
{
    Boot_CmdTypeDef request_cmd;
    Boot_ResultTypeDef result;
    Boot_StartInfoTypeDef start_info;
    Boot_DataInfoTypeDef data_info;
    uint32_t packet_count;
    uint32_t response_value;

    request_cmd = (Boot_CmdTypeDef)Boot_ParseCmd(frame);
    result = BOOT_RESULT_FRAME_ERROR;
    response_value = 0U;

    if (Boot_VerifyFrameCRC(frame, frame_length) == 0U)
    {
        printf("TCP FRAME: bad crc cmd=0x%04X\r\n",
               (unsigned int)request_cmd);
        return TcpServer_SendResponse(client,
                                      request_cmd,
                                      BOOT_RESULT_FRAME_ERROR,
                                      0U);
    }

    switch (request_cmd)
    {
        case CMD_START_UPDATE:
            if (Boot_ParseStartFrame(frame, frame_length, &start_info) != 0U)
            {
                result = AppUpdate_HandleStart(&start_info, &response_value);
                if (result == BOOT_RESULT_OK)
                {
                    printf("TCP START: target=%u size=%lu crc=0x%04X version=0x%08lX\r\n",
                           (unsigned int)start_info.target,
                           (unsigned long)start_info.image_size,
                           (unsigned int)start_info.image_crc,
                           (unsigned long)(uint32_t)start_info.image_version);
                }
            }
            break;

        case CMD_DATA_PACKET:
            if (Boot_ParseDataFrame(frame, frame_length, &data_info) != 0U)
            {
                result = AppUpdate_HandleData(&data_info, &response_value);
                if (result == BOOT_RESULT_OK)
                {
                    printf("TCP DATA: sequence=%lu length=%u\r\n",
                           (unsigned long)data_info.sequence,
                           (unsigned int)data_info.data_len);
                }
            }
            break;

        case CMD_END_UPDATE:
            if (Boot_ParseEndFrame(frame, frame_length, &packet_count) != 0U)
            {
                result = AppUpdate_HandleEnd(packet_count, &response_value);
                if (result == BOOT_RESULT_OK)
                {
                    printf("TCP END: packet_count=%lu\r\n",
                           (unsigned long)packet_count);
                }
            }
            break;

        default:
            result = BOOT_RESULT_UNKNOWN_CMD;
            printf("TCP CMD: unsupported=0x%04X\r\n",
                   (unsigned int)request_cmd);
            break;
    }

    if (result != BOOT_RESULT_OK)
    {
        printf("TCP FRAME: rejected cmd=0x%04X result=0x%04X\r\n",
               (unsigned int)request_cmd,
               (unsigned int)result);
    }

    return TcpServer_SendResponse(client,
                                  request_cmd,
                                  result,
                                  response_value);
}

static err_t TcpServer_InputData(struct netconn *client,
                                 Boot_RxContextTypeDef *rx_context,
                                 const uint8_t *data,
                                 uint16_t data_length)
{
    Boot_RxResultTypeDef rx_result;
    uint8_t *frame;
    uint16_t frame_length;
    uint16_t index;
    err_t err;

    for (index = 0U; index < data_length; index++)
    {
        rx_result = Boot_RxInputByte(rx_context, data[index]);

        if (rx_result == BOOT_RX_FRAME_ERROR)
        {
            printf("TCP FRAME: invalid length\r\n");
            continue;
        }

        if (rx_result == BOOT_RX_FRAME_READY)
        {
            if (Boot_RxGetFrame(rx_context, &frame, &frame_length) == 0U)
            {
                Boot_RxInit(rx_context);
                continue;
            }

            err = TcpServer_ProcessFrame(client, frame, frame_length);
            Boot_RxInit(rx_context);

            if (err != ERR_OK)
            {
                return err;
            }
        }
    }

    return ERR_OK;
}

void TcpServerTask(void *argument)
{
    struct netconn *listener;
    struct netconn *client;
    struct netbuf *rx_buffer;
    void *rx_data;
    u16_t rx_length;
    err_t err;
    Boot_RxContextTypeDef rx_context;

    (void)argument;

    /*创建对象*/
    listener = netconn_new(NETCONN_TCP);
    if (listener == NULL)
    {
        printf("TCP: create failed\r\n");
        osThreadExit();
    }

    /*绑定端口*/
    err = netconn_bind(listener, IP_ADDR_ANY, TCP_SERVER_PORT);
    if (err != ERR_OK)
    {
        printf("TCP: bind failed: %d\r\n", (int)err);
        netconn_delete(listener);
        osThreadExit();
    }

    /*监听连接*/
    err = netconn_listen(listener);
    if (err != ERR_OK)
    {
        printf("TCP: listen failed: %d\r\n", (int)err);
        netconn_delete(listener);
        osThreadExit();
    }

    printf("TCP: listening on port %u\r\n", TCP_SERVER_PORT);

    AppUpdate_Init();

    for (;;)
    {
        client = NULL;
        /*等待连接*/
        err = netconn_accept(listener, &client);
        if ((err != ERR_OK) || (client == NULL))
        {
            osDelay(100U);
            continue;
        }

        printf("TCP: client connected\r\n");
        AppUpdate_Init();
        Boot_RxInit(&rx_context);
        rx_buffer = NULL;

        for (;;)
        {
            /*接收数据*/
            err = netconn_recv(client, &rx_buffer);
            if ((err != ERR_OK) || (rx_buffer == NULL))
            {
                break;
            }

            netbuf_first(rx_buffer);
            do
            {
                if (netbuf_data(rx_buffer, &rx_data, &rx_length) == ERR_OK)
                {
                    err = TcpServer_InputData(client,
                                              &rx_context,
                                              (const uint8_t *)rx_data,
                                              rx_length);
                    if (err != ERR_OK)
                    {
                        break;
                    }
                }
            } while (netbuf_next(rx_buffer) >= 0);

            netbuf_delete(rx_buffer);
            rx_buffer = NULL;

            if (err != ERR_OK)
            {
                break;
            }
        }

        if (rx_buffer != NULL)
        {
            netbuf_delete(rx_buffer);
            rx_buffer = NULL;
        }

        netconn_close(client);
        netconn_delete(client);
        AppUpdate_Abort();
        printf("TCP: client disconnected\r\n");
    }
}
