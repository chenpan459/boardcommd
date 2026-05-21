#include "transport.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

static speed_t baud_to_speed(int baudrate)
{
    switch (baudrate) {
    case 9600:
        return B9600;
    case 57600:
        return B57600;
    case 115200:
    default:
        return B115200;
    }
}

static int uart_open(bc_transport_t *transport)
{
    struct termios tio;

    transport->fd = open(transport->config.endpoint, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (transport->fd < 0) {
        return BC_ERR_IO;
    }

    memset(&tio, 0, sizeof(tio));
    cfmakeraw(&tio);
    cfsetispeed(&tio, baud_to_speed(transport->config.baudrate));
    cfsetospeed(&tio, baud_to_speed(transport->config.baudrate));
    tio.c_cflag |= CLOCAL | CREAD;

    if (tcsetattr(transport->fd, TCSANOW, &tio) < 0) {
        close(transport->fd);
        transport->fd = -1;
        return BC_ERR_IO;
    }

    return BC_OK;
}

static void uart_close(bc_transport_t *transport)
{
    if (transport->fd >= 0) {
        close(transport->fd);
        transport->fd = -1;
    }
}

static int uart_send(bc_transport_t *transport, const uint8_t *data, size_t len)
{
    size_t off = 0;

    while (off < len) {
        ssize_t n = write(transport->fd, data + off, len - off);

        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        return BC_ERR_IO;
    }
    (void)tcdrain(transport->fd);
    return BC_OK;
}

static const bc_transport_ops_t uart_ops = {
    .open = uart_open,
    .close = uart_close,
    .send = uart_send,
};

int bc_uart_transport_init(bc_transport_t *transport, const bc_transport_config_t *cfg)
{
    memset(transport, 0, sizeof(*transport));
    snprintf(transport->name, sizeof(transport->name), "%s", cfg->name);
    transport->type = BC_TRANSPORT_UART;
    transport->fd = -1;
    transport->config = *cfg;
    transport->ops = &uart_ops;
    return BC_OK;
}
