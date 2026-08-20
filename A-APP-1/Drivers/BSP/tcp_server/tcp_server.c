#include "tcp_server.h"

#include "cmsis_os.h"
#include "lwip/api.h"

#include <stdio.h>

#define TCP_SERVER_PORT 5000U

void TcpServerTask(void *argument)
{
    struct netconn *listener;
    struct netconn *client;
    struct netbuf *rx_buffer;
    void *rx_data;
    u16_t rx_length;
    err_t err;

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
                    printf("TCP RX: %.*s\r\n", (int)rx_length, (char *)rx_data);
                    err = netconn_write(client, rx_data, rx_length, NETCONN_COPY);
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
        printf("TCP: client disconnected\r\n");
    }
}
