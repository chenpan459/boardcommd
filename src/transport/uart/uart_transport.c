#include "transport.h"

#include "protocol.h"
#include "reactor.h"
#include "bc_log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <termios.h>
#include <unistd.h>

#define BC_UART_TAG "uart"
#define BC_UART_SEND_CAP (BC_MAX_FRAME_LEN * 8)
#define BC_UART_REOPEN_MS_INIT 1000
#define BC_UART_REOPEN_MS_MAX 30000
#define BC_UART_GPIO_PATH_LEN 64

typedef struct {
    int mode;
    int de_gpio;
    int de_value_fd;
    int de_tx_active;
    int timer_fd;
    int reopen_armed;
    int retry_attempt;
    int opened;
    uint8_t *send_buf;
    size_t send_len;
    size_t send_off;
    bc_reactor_t *reactor;
    bc_reactor_cb event_cb;
    void *event_user;
} bc_uart_impl_t;

static bc_uart_impl_t *uart_impl(bc_transport_t *transport)
{
    return transport != NULL ? (bc_uart_impl_t *)transport->impl : NULL;
}

static speed_t baud_to_speed(int baudrate)
{
    switch (baudrate) {
    case 9600:
        return B9600;
    case 38400:
        return B38400;
    case 57600:
        return B57600;
    case 230400:
        return B230400;
    case 460800:
        return B460800;
    case 921600:
        return B921600;
    case 115200:
    default:
        return B115200;
    }
}

static uint32_t uart_epoll_events(const bc_uart_impl_t *impl)
{
    uint32_t events = EPOLLIN | EPOLLHUP | EPOLLERR;

    if (impl->send_off < impl->send_len) {
        events |= EPOLLOUT;
    }
    return events;
}

static int uart_watch_fd(bc_transport_t *transport, uint32_t events)
{
    bc_uart_impl_t *impl = uart_impl(transport);

    if (impl == NULL || impl->reactor == NULL || transport->fd < 0) {
        return BC_OK;
    }
    if (bc_reactor_mod(impl->reactor, transport->fd, events, impl->event_cb, impl->event_user) == BC_OK) {
        return BC_OK;
    }
    return bc_reactor_add(impl->reactor, transport->fd, events, impl->event_cb, impl->event_user);
}

static int gpio_export(int pin)
{
    char path[BC_UART_GPIO_PATH_LEN];
    int fd;

    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d", pin);
    if (access(path, F_OK) == 0) {
        return BC_OK;
    }

    fd = open("/sys/class/gpio/export", O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        return BC_ERR_IO;
    }
    if (dprintf(fd, "%d", pin) < 0 && errno != EBUSY) {
        close(fd);
        return BC_ERR_IO;
    }
    close(fd);
    return BC_OK;
}

static int gpio_open_value(int pin)
{
    char path[BC_UART_GPIO_PATH_LEN];
    int fd;

    if (gpio_export(pin) != BC_OK) {
        return -1;
    }

    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", pin);
    fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd >= 0) {
        (void)write(fd, "out", 3);
        close(fd);
    }

    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", pin);
    fd = open(path, O_WRONLY | O_CLOEXEC);
    return fd;
}

static int gpio_set_value(int value_fd, int level)
{
    const char ch = level ? '1' : '0';

    if (value_fd < 0) {
        return BC_OK;
    }
    if (lseek(value_fd, 0, SEEK_SET) < 0) {
        return BC_ERR_IO;
    }
    if (write(value_fd, &ch, 1) != 1) {
        return BC_ERR_IO;
    }
    return BC_OK;
}

static void uart_rs485_deinit(bc_uart_impl_t *impl)
{
    if (impl->de_tx_active) {
        (void)gpio_set_value(impl->de_value_fd, 0);
        impl->de_tx_active = 0;
    }
    if (impl->de_value_fd >= 0) {
        close(impl->de_value_fd);
        impl->de_value_fd = -1;
    }
}

static int uart_rs485_init(bc_uart_impl_t *impl)
{
    if (impl->mode != BC_UART_MODE_485) {
        return BC_OK;
    }
    if (impl->de_gpio < 0) {
        BC_LOGW(BC_UART_TAG, "RS-485 mode without de_gpio, assuming auto-direction adapter");
        return BC_OK;
    }

    impl->de_value_fd = gpio_open_value(impl->de_gpio);
    if (impl->de_value_fd < 0) {
        BC_LOGE(BC_UART_TAG, "failed to open DE gpio %d", impl->de_gpio);
        return BC_ERR_IO;
    }
    if (gpio_set_value(impl->de_value_fd, 0) != BC_OK) {
        uart_rs485_deinit(impl);
        return BC_ERR_IO;
    }
    return BC_OK;
}

static int uart_rs485_tx_enable(bc_transport_t *transport)
{
    bc_uart_impl_t *impl = uart_impl(transport);

    if (impl == NULL || impl->mode != BC_UART_MODE_485 || impl->de_tx_active) {
        return BC_OK;
    }
    if (gpio_set_value(impl->de_value_fd, 1) != BC_OK) {
        return BC_ERR_IO;
    }
    impl->de_tx_active = 1;
    return BC_OK;
}

static void uart_rs485_tx_disable(bc_transport_t *transport)
{
    bc_uart_impl_t *impl = uart_impl(transport);

    if (impl == NULL || impl->mode != BC_UART_MODE_485 || !impl->de_tx_active) {
        return;
    }
    if (transport->fd >= 0) {
        (void)tcdrain(transport->fd);
    }
    (void)gpio_set_value(impl->de_value_fd, 0);
    impl->de_tx_active = 0;
}

static int uart_apply_termios(int fd, int baudrate)
{
    struct termios tio;

    if (tcgetattr(fd, &tio) != 0) {
        memset(&tio, 0, sizeof(tio));
        cfmakeraw(&tio);
    } else {
        cfmakeraw(&tio);
    }

    cfsetispeed(&tio, baud_to_speed(baudrate));
    cfsetospeed(&tio, baud_to_speed(baudrate));
    tio.c_cflag |= (CLOCAL | CREAD);
    tio.c_cflag &= ~(PARENB | CSTOPB | CSIZE);
    tio.c_cflag |= CS8;
    tio.c_iflag &= ~(IXON | IXOFF | IXANY);
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;

    return tcsetattr(fd, TCSANOW, &tio) == 0 ? BC_OK : BC_ERR_IO;
}

static int uart_device_open(bc_transport_t *transport)
{
    bc_uart_impl_t *impl = uart_impl(transport);
    const char *path = transport->config.endpoint;

    transport->fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (transport->fd < 0) {
        return BC_ERR_IO;
    }
    if (uart_apply_termios(transport->fd, transport->config.baudrate) != BC_OK) {
        close(transport->fd);
        transport->fd = -1;
        return BC_ERR_IO;
    }

    impl->opened = 1;
    BC_LOGI(
        BC_UART_TAG,
        "%s: opened %s baud=%d mode=%s de_gpio=%d",
        transport->name,
        path,
        transport->config.baudrate,
        impl->mode == BC_UART_MODE_485 ? "RS-485" : "RS-232",
        impl->de_gpio);
    return BC_OK;
}

static void uart_device_close(bc_transport_t *transport)
{
    bc_uart_impl_t *impl = uart_impl(transport);

    uart_rs485_tx_disable(transport);
    if (transport->fd >= 0) {
        if (impl != NULL && impl->reactor != NULL) {
            bc_reactor_del(impl->reactor, transport->fd);
        }
        close(transport->fd);
        transport->fd = -1;
    }
    if (impl != NULL) {
        impl->opened = 0;
    }
}

static int uart_reopen_delay_ms(bc_uart_impl_t *impl)
{
    int delay = BC_UART_REOPEN_MS_INIT;

    if (impl->retry_attempt > 0) {
        delay = BC_UART_REOPEN_MS_INIT << (impl->retry_attempt - 1);
        if (delay > BC_UART_REOPEN_MS_MAX) {
            delay = BC_UART_REOPEN_MS_MAX;
        }
    }
    return delay;
}

static int uart_timer_arm(bc_transport_t *transport, int delay_ms)
{
    bc_uart_impl_t *impl = uart_impl(transport);
    struct itimerspec ts;

    if (impl == NULL || impl->timer_fd < 0) {
        return BC_ERR_IO;
    }

    memset(&ts, 0, sizeof(ts));
    ts.it_value.tv_sec = (time_t)(delay_ms / 1000);
    ts.it_value.tv_nsec = (long)(delay_ms % 1000) * 1000000L;
    return timerfd_settime(impl->timer_fd, 0, &ts, NULL) == 0 ? BC_OK : BC_ERR_IO;
}

static void uart_schedule_reopen(bc_transport_t *transport)
{
    bc_uart_impl_t *impl = uart_impl(transport);
    int delay;

    if (impl == NULL) {
        return;
    }

    uart_device_close(transport);
    impl->reopen_armed = 1;
    delay = uart_reopen_delay_ms(impl);
    impl->retry_attempt++;
    (void)uart_timer_arm(transport, delay);
    BC_LOGW(BC_UART_TAG, "%s: reopen in %d ms (attempt %d)", transport->name, delay, impl->retry_attempt);
}

static int uart_try_reopen(bc_transport_t *transport)
{
    bc_uart_impl_t *impl = uart_impl(transport);

    if (uart_device_open(transport) != BC_OK) {
        uart_schedule_reopen(transport);
        return BC_ERR_IO;
    }

    impl->retry_attempt = 0;
    impl->reopen_armed = 0;
    return uart_watch_fd(transport, uart_epoll_events(impl));
}

static void uart_clear_send_queue(bc_uart_impl_t *impl)
{
    impl->send_len = 0;
    impl->send_off = 0;
}

static int uart_ensure_send_cap(bc_uart_impl_t *impl, size_t need)
{
    if (need > BC_UART_SEND_CAP) {
        return BC_ERR_NOMEM;
    }
    if (impl->send_buf == NULL) {
        impl->send_buf = malloc(BC_UART_SEND_CAP);
        if (impl->send_buf == NULL) {
            return BC_ERR_NOMEM;
        }
    }
    return BC_OK;
}

static int uart_flush_send(bc_transport_t *transport)
{
    bc_uart_impl_t *impl = uart_impl(transport);

    if (impl == NULL || !impl->opened || transport->fd < 0) {
        return BC_ERR_IO;
    }

    if (impl->send_off < impl->send_len && uart_rs485_tx_enable(transport) != BC_OK) {
        return BC_ERR_IO;
    }

    while (impl->send_off < impl->send_len) {
        ssize_t n = write(
            transport->fd,
            impl->send_buf + impl->send_off,
            impl->send_len - impl->send_off);

        if (n > 0) {
            impl->send_off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return uart_watch_fd(transport, uart_epoll_events(impl));
        }
        uart_schedule_reopen(transport);
        return BC_ERR_IO;
    }

    uart_clear_send_queue(impl);
    uart_rs485_tx_disable(transport);
    return uart_watch_fd(transport, uart_epoll_events(impl));
}

static int uart_queue_send(bc_transport_t *transport, const uint8_t *data, size_t len)
{
    bc_uart_impl_t *impl = uart_impl(transport);
    size_t need;

    if (impl == NULL || !impl->opened) {
        return BC_ERR_IO;
    }

    need = (impl->send_len - impl->send_off) + len;
    if (uart_ensure_send_cap(impl, need) != BC_OK) {
        return BC_ERR_NOMEM;
    }

    memcpy(impl->send_buf + impl->send_len, data, len);
    impl->send_len += len;
    return uart_watch_fd(transport, uart_epoll_events(impl));
}

static int uart_open(bc_transport_t *transport)
{
    bc_uart_impl_t *impl = uart_impl(transport);

    if (impl == NULL) {
        return BC_ERR_INVALID;
    }

    impl->mode = transport->config.uart_mode;
    impl->de_gpio = transport->config.rs485_de_gpio;
    impl->de_value_fd = -1;
    impl->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (impl->timer_fd < 0) {
        return BC_ERR_IO;
    }

    if (uart_rs485_init(impl) != BC_OK) {
        close(impl->timer_fd);
        impl->timer_fd = -1;
        return BC_ERR_IO;
    }

    if (uart_device_open(transport) != BC_OK) {
        uart_rs485_deinit(impl);
        close(impl->timer_fd);
        impl->timer_fd = -1;
        return BC_ERR_IO;
    }

    return BC_OK;
}

static void uart_close(bc_transport_t *transport)
{
    bc_uart_impl_t *impl = uart_impl(transport);

    if (impl == NULL) {
        return;
    }

    uart_device_close(transport);
    uart_rs485_deinit(impl);

    if (impl->timer_fd >= 0) {
        if (impl->reactor != NULL) {
            bc_reactor_del(impl->reactor, impl->timer_fd);
        }
        close(impl->timer_fd);
        impl->timer_fd = -1;
    }

    free(impl->send_buf);
    impl->send_buf = NULL;
    free(impl);
    transport->impl = NULL;
    transport->fd = -1;
}

static int uart_send(bc_transport_t *transport, const uint8_t *data, size_t len)
{
    bc_uart_impl_t *impl = uart_impl(transport);
    size_t off = 0;

    if (impl == NULL || !impl->opened || transport->fd < 0) {
        return BC_ERR_IO;
    }

    if (impl->send_off < impl->send_len) {
        return uart_queue_send(transport, data, len);
    }

    if (uart_rs485_tx_enable(transport) != BC_OK) {
        return BC_ERR_IO;
    }

    while (off < len) {
        ssize_t n = write(transport->fd, data + off, len - off);

        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return uart_queue_send(transport, data + off, len - off);
        }
        uart_schedule_reopen(transport);
        return BC_ERR_IO;
    }

    uart_rs485_tx_disable(transport);
    return BC_OK;
}

static int uart_recv(bc_transport_t *transport, uint8_t *buf, size_t cap, size_t *out_len)
{
    bc_uart_impl_t *impl = uart_impl(transport);
    ssize_t n;

    if (impl == NULL || !impl->opened || transport->fd < 0 || out_len == NULL) {
        return BC_ERR_INVALID;
    }

    if (impl->mode == BC_UART_MODE_485 && impl->de_tx_active) {
        *out_len = 0;
        return BC_ERR_NOT_FOUND;
    }

    n = read(transport->fd, buf, cap);
    if (n > 0) {
        *out_len = (size_t)n;
        return BC_OK;
    }
    if (n == 0) {
        uart_schedule_reopen(transport);
        return BC_ERR_IO;
    }
    if (errno == EINTR) {
        *out_len = 0;
        return BC_ERR_NOT_FOUND;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        *out_len = 0;
        return BC_ERR_NOT_FOUND;
    }

    uart_schedule_reopen(transport);
    return BC_ERR_IO;
}

static int uart_bind_reactor(bc_transport_t *transport, bc_reactor_t *reactor, bc_reactor_cb cb, void *user)
{
    bc_uart_impl_t *impl = uart_impl(transport);

    if (impl == NULL) {
        return BC_ERR_INVALID;
    }

    impl->reactor = reactor;
    impl->event_cb = cb;
    impl->event_user = user;

    if (transport->fd >= 0) {
        if (uart_watch_fd(transport, uart_epoll_events(impl)) != BC_OK) {
            return BC_ERR_IO;
        }
    }
    if (impl->timer_fd >= 0) {
        if (bc_reactor_add(reactor, impl->timer_fd, EPOLLIN, cb, user) != BC_OK) {
            return BC_ERR_IO;
        }
    }
    return BC_OK;
}

static int uart_get_fds(bc_transport_t *transport, int *fds, size_t *count)
{
    (void)transport;
    (void)fds;
    if (count != NULL) {
        *count = 0;
    }
    return BC_OK;
}

static int uart_handle_event(bc_transport_t *transport, int fd, uint32_t events)
{
    bc_uart_impl_t *impl = uart_impl(transport);

    if (impl == NULL) {
        return BC_ERR_INVALID;
    }

    if (fd == impl->timer_fd && (events & EPOLLIN) != 0) {
        uint64_t exp;

        while (read(impl->timer_fd, &exp, sizeof(exp)) == (ssize_t)sizeof(exp)) {
            if (impl->reopen_armed) {
                impl->reopen_armed = 0;
                (void)uart_try_reopen(transport);
            }
        }
        return BC_OK;
    }

    if (fd == transport->fd) {
        if ((events & (EPOLLHUP | EPOLLERR)) != 0) {
            uart_schedule_reopen(transport);
            return BC_OK;
        }
        if ((events & EPOLLOUT) != 0 && impl->send_off < impl->send_len) {
            (void)uart_flush_send(transport);
        }
    }

    return BC_OK;
}

static const bc_transport_ops_t uart_ops = {
    .open = uart_open,
    .close = uart_close,
    .send = uart_send,
    .recv = uart_recv,
    .get_fds = uart_get_fds,
    .bind_reactor = uart_bind_reactor,
    .handle_event = uart_handle_event,
};

int bc_uart_transport_init(bc_transport_t *transport, const bc_transport_config_t *cfg)
{
    bc_uart_impl_t *impl;

    memset(transport, 0, sizeof(*transport));
    snprintf(transport->name, sizeof(transport->name), "%s", cfg->name);
    transport->type = BC_TRANSPORT_UART;
    transport->fd = -1;
    transport->config = *cfg;
    impl = calloc(1, sizeof(*impl));
    if (impl == NULL) {
        return BC_ERR_NOMEM;
    }
    impl->de_value_fd = -1;
    impl->timer_fd = -1;
    transport->impl = impl;
    transport->ops = &uart_ops;
    return BC_OK;
}
