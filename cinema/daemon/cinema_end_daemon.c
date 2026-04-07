// SPDX-License-Identifier: Apache-2.0
/*
 * cinema_end_daemon.c — Userspace bridge between cinema_end.ko and the
 *                       STM32U5 MCU over USB-CDC (ttyACMx).
 *
 * Architecture: single-threaded epoll event loop, no threads.
 *
 * Copyright (c) 2024, Xiaomi Cinema Kernel Project
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <android/log.h>

#include "cinema_protocol.h"

/* ------------------------------------------------------------------ */
/* Logging                                                              */
/* ------------------------------------------------------------------ */

#define LOG_TAG "cinema_end"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/* ------------------------------------------------------------------ */
/* Sysfs / device paths                                                 */
/* ------------------------------------------------------------------ */

#define DEV_CINEMA_END          "/dev/cinema_end"
#define SYSFS_BASE              "/sys/kernel/cinema_end"
#define SYSFS_ND_SETPOINT       SYSFS_BASE "/nd_setpoint"
#define SYSFS_MODE              SYSFS_BASE "/mode"
#define SYSFS_ENABLE            SYSFS_BASE "/enable"
#define SYSFS_ND_ACTUAL         SYSFS_BASE "/nd_actual"
#define SYSFS_CELL_TEMP         SYSFS_BASE "/cell_temp"

#define SYS_TTY_CLASS           "/sys/class/tty"
#define DEV_DIR                 "/dev"

/* STM32 USB-CDC identifiers */
#define MCU_ID_VENDOR           "0483"
#define MCU_ID_PRODUCT          "5740"

/* epoll capacity */
#define EPOLL_MAX_EVENTS        16

/* Heartbeat interval in milliseconds */
#define HEARTBEAT_MS            500

/* Maximum ttyACM entries to scan */
#define MAX_TTY_SCAN            32

/* ------------------------------------------------------------------ */
/* Global state                                                         */
/* ------------------------------------------------------------------ */

static int g_cinema_fd   = -1;   /* /dev/cinema_end               */
static int g_tty_fd      = -1;   /* /dev/ttyACMx                  */
static int g_epoll_fd    = -1;
static int g_signal_fd   = -1;
static int g_inotify_fd  = -1;
static int g_timer_fd    = -1;

/* Sysfs watch fds (O_RDONLY, watched with EPOLLPRI) */
static int g_nd_fd       = -1;
static int g_mode_fd     = -1;
static int g_enable_fd   = -1;

/* inotify watch descriptor for /dev */
static int g_inotify_wd  = -1;

/* tty path resolved from sysfs scan */
static char g_tty_path[PATH_MAX];

/* Receive buffer for MCU frames */
#define RX_BUF_SIZE 512
static uint8_t g_rx_buf[RX_BUF_SIZE];
static size_t  g_rx_len = 0;

/* ------------------------------------------------------------------ */
/* Utility: sysfs read / write helpers                                  */
/* ------------------------------------------------------------------ */

/*
 * sysfs_read_int32 - read a decimal int32_t from a sysfs file.
 * Returns 0 on success, -1 on error.
 */
static int sysfs_read_int32(const char *path, int32_t *out)
{
    char buf[32];
    int fd;
    ssize_t n;
    long val;

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        LOGE("sysfs_read_int32: open %s: %s", path, strerror(errno));
        return -1;
    }

    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);

    if (n <= 0) {
        LOGE("sysfs_read_int32: read %s: %s", path, strerror(errno));
        return -1;
    }

    buf[n] = '\0';
    errno = 0;
    val = strtol(buf, NULL, 10);
    if (errno) {
        LOGE("sysfs_read_int32: strtol %s: %s", path, strerror(errno));
        return -1;
    }

    *out = (int32_t)val;
    return 0;
}

/*
 * sysfs_write_int32 - write a decimal int32_t to a sysfs file.
 * Returns 0 on success, -1 on error.
 */
static int sysfs_write_int32(const char *path, int32_t val)
{
    char buf[32];
    int fd;
    ssize_t n;
    int len;

    len = snprintf(buf, sizeof(buf), "%" PRId32 "\n", val);
    if (len <= 0) {
        LOGE("sysfs_write_int32: snprintf failed");
        return -1;
    }

    fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        LOGE("sysfs_write_int32: open %s: %s", path, strerror(errno));
        return -1;
    }

    n = write(fd, buf, (size_t)len);
    close(fd);

    if (n != len) {
        LOGE("sysfs_write_int32: write %s: %s", path, strerror(errno));
        return -1;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* epoll helpers                                                        */
/* ------------------------------------------------------------------ */

static int epoll_add(int epfd, int fd, uint32_t events)
{
    struct epoll_event ev;
    ev.events  = events;
    ev.data.fd = fd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        LOGE("epoll_ctl ADD fd=%d: %s", fd, strerror(errno));
        return -1;
    }
    return 0;
}

static int epoll_del(int epfd, int fd)
{
    if (epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL) < 0) {
        LOGE("epoll_ctl DEL fd=%d: %s", fd, strerror(errno));
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* MCU tty discovery (no libudev — scan sysfs manually)                 */
/* ------------------------------------------------------------------ */

/*
 * match_mcu_tty - check whether /sys/class/tty/<name> belongs to our MCU.
 *
 * Walks the symlink ../../../ path chain to find idVendor / idProduct.
 * Returns 1 if match, 0 otherwise.
 */
static int match_mcu_tty(const char *tty_name)
{
    char uevent_path[PATH_MAX];
    char link_buf[PATH_MAX];
    char resolved[PATH_MAX];
    char line[128];
    ssize_t n;
    FILE *f;
    int match_vendor = 0, match_product = 0;

    /*
     * /sys/class/tty/ttyACM0 → ../../devices/platform/.../ttyACM0
     * We need to walk up to the USB interface parent to find idVendor.
     * Build: /sys/class/tty/<name> → readlink → resolve → ../../../ uevent
     */
    snprintf(uevent_path, sizeof(uevent_path),
             SYS_TTY_CLASS "/%s", tty_name);

    n = readlink(uevent_path, link_buf, sizeof(link_buf) - 1);
    if (n < 0)
        return 0;
    link_buf[n] = '\0';

    /*
     * link_buf is relative to /sys/class/tty/.
     * Resolve to absolute path.
     */
    snprintf(resolved, sizeof(resolved), SYS_TTY_CLASS "/%s", link_buf);

    /*
     * The ttyACMx sysfs node lives at:
     *   .../usb1/1-1/1-1:1.0/tty/ttyACM0
     * idVendor is two levels above the interface dir (1-1:1.0 → 1-1).
     * Walk up 3 directories from the ttyACMx node: tty → interface → device.
     */
    char *p;
    char usb_dev[PATH_MAX];

    strncpy(usb_dev, resolved, sizeof(usb_dev) - 1);
    usb_dev[sizeof(usb_dev) - 1] = '\0';

    /* strip trailing /ttyACMx */
    p = strrchr(usb_dev, '/');
    if (p) *p = '\0';
    /* strip /tty */
    p = strrchr(usb_dev, '/');
    if (p) *p = '\0';
    /* strip /interface (e.g. 1-1:1.0) */
    p = strrchr(usb_dev, '/');
    if (p) *p = '\0';

    /* Now usb_dev should point to the USB device directory (e.g. 1-1) */
    snprintf(uevent_path, sizeof(uevent_path), "%s/idVendor", usb_dev);
    f = fopen(uevent_path, "r");
    if (f) {
        if (fgets(line, sizeof(line), f)) {
            /* strip newline */
            line[strcspn(line, "\n")] = '\0';
            if (strcmp(line, MCU_ID_VENDOR) == 0)
                match_vendor = 1;
        }
        fclose(f);
    }

    snprintf(uevent_path, sizeof(uevent_path), "%s/idProduct", usb_dev);
    f = fopen(uevent_path, "r");
    if (f) {
        if (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\n")] = '\0';
            if (strcmp(line, MCU_ID_PRODUCT) == 0)
                match_product = 1;
        }
        fclose(f);
    }

    return (match_vendor && match_product);
}

/*
 * find_mcu_tty - scan /sys/class/tty/ttyACM* for our MCU.
 *
 * Fills g_tty_path on success.
 * Returns 0 on success, -1 if not found.
 */
static int find_mcu_tty(void)
{
    char name_buf[64];
    int i;

    for (i = 0; i < MAX_TTY_SCAN; i++) {
        snprintf(name_buf, sizeof(name_buf), "ttyACM%d", i);

        /* Check sysfs symlink exists */
        char sysfs_node[PATH_MAX];
        snprintf(sysfs_node, sizeof(sysfs_node),
                 SYS_TTY_CLASS "/%s", name_buf);

        if (access(sysfs_node, F_OK) != 0)
            continue;

        if (match_mcu_tty(name_buf)) {
            snprintf(g_tty_path, sizeof(g_tty_path), "/dev/%s", name_buf);
            LOGI("find_mcu_tty: found MCU at %s", g_tty_path);
            return 0;
        }
    }

    LOGW("find_mcu_tty: MCU not found");
    return -1;
}

/* ------------------------------------------------------------------ */
/* TTY setup                                                            */
/* ------------------------------------------------------------------ */

/*
 * open_tty - open g_tty_path and configure for raw binary I/O at 115200.
 * Returns fd on success, -1 on error.
 */
static int open_tty(const char *path)
{
    struct termios tio;
    int fd;

    fd = open(path, O_RDWR | O_NOCTTY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        LOGE("open_tty: open %s: %s", path, strerror(errno));
        return -1;
    }

    if (tcgetattr(fd, &tio) < 0) {
        LOGE("open_tty: tcgetattr %s: %s", path, strerror(errno));
        close(fd);
        return -1;
    }

    cfmakeraw(&tio);
    cfsetispeed(&tio, B115200);
    cfsetospeed(&tio, B115200);

    /* One byte at a time, no timeout (non-blocking reads) */
    tio.c_cc[VMIN]  = 0;
    tio.c_cc[VTIME] = 0;

    if (tcsetattr(fd, TCSANOW, &tio) < 0) {
        LOGE("open_tty: tcsetattr %s: %s", path, strerror(errno));
        close(fd);
        return -1;
    }

    LOGI("open_tty: opened %s fd=%d", path, fd);
    return fd;
}

/* ------------------------------------------------------------------ */
/* Frame transmission                                                   */
/* ------------------------------------------------------------------ */

static int send_frame(uint8_t cmd, const void *payload, uint8_t payload_len)
{
    uint8_t frame[FRAME_MAX_LEN];
    int frame_len;
    ssize_t n;

    if (g_tty_fd < 0) {
        LOGW("send_frame: tty not open, dropping cmd=0x%02x", cmd);
        return -1;
    }

    frame_len = cinema_build_frame(frame, cmd, payload, payload_len);

    n = write(g_tty_fd, frame, (size_t)frame_len);
    if (n != frame_len) {
        LOGE("send_frame: write cmd=0x%02x: %s", cmd, strerror(errno));
        return -1;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Frame reception / parsing                                            */
/* ------------------------------------------------------------------ */

/*
 * process_frame - handle a fully validated inbound frame.
 */
static void process_frame(uint8_t cmd, const uint8_t *payload, uint8_t len)
{
    switch (cmd) {

    case CMD_TELEMETRY: {
        if (len < (int)sizeof(struct telem_payload)) {
            LOGW("CMD_TELEMETRY: short payload len=%u", len);
            break;
        }
        struct telem_payload tp;
        memcpy(&tp, payload, sizeof(tp));
        LOGI("CMD_TELEMETRY: nd_mb=%" PRId32 " temp_mc=%" PRId32,
             tp.nd_mb, tp.temp_mc);
        if (sysfs_write_int32(SYSFS_ND_ACTUAL, tp.nd_mb) < 0)
            LOGW("CMD_TELEMETRY: failed to write nd_actual");
        if (sysfs_write_int32(SYSFS_CELL_TEMP, tp.temp_mc) < 0)
            LOGW("CMD_TELEMETRY: failed to write cell_temp");
        break;
    }

    case CMD_FAULT: {
        uint8_t reason = (len >= 1) ? payload[0] : 0xFF;
        LOGE("CMD_FAULT: MCU fault reason=0x%02x", reason);
        /* Kernel handles threshold; cell_temp write above already triggers it */
        break;
    }

    case CMD_ACK: {
        uint8_t acked = (len >= 1) ? payload[0] : 0xFF;
        LOGI("CMD_ACK: acked_cmd=0x%02x", acked);
        break;
    }

    case CMD_NACK: {
        uint8_t failed = (len >= 1) ? payload[0] : 0xFF;
        uint8_t reason = (len >= 2) ? payload[1] : 0xFF;
        LOGW("CMD_NACK: failed_cmd=0x%02x reason=0x%02x", failed, reason);
        break;
    }

    default:
        LOGW("process_frame: unknown cmd=0x%02x len=%u", cmd, len);
        break;
    }
}

/*
 * drain_tty - consume all available bytes from g_tty_fd, reassemble frames,
 *             dispatch process_frame() for each complete, valid frame.
 *
 * Returns 0 on success, -1 on I/O error (caller must handle disconnect).
 */
static int drain_tty(void)
{
    uint8_t tmp[256];
    ssize_t n;

    while (1) {
        n = read(g_tty_fd, tmp, sizeof(tmp));
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break; /* no more data */
            LOGE("drain_tty: read: %s", strerror(errno));
            return -1;
        }
        if (n == 0) {
            LOGE("drain_tty: EOF on tty");
            return -1;
        }

        /* Append to reassembly buffer */
        if (g_rx_len + (size_t)n > sizeof(g_rx_buf)) {
            /* Buffer overrun — resync by discarding */
            LOGW("drain_tty: rx buffer overrun, discarding");
            g_rx_len = 0;
        }
        memcpy(g_rx_buf + g_rx_len, tmp, (size_t)n);
        g_rx_len += (size_t)n;
    }

    /* Parse frames out of reassembly buffer */
    size_t consumed = 0;
    while (consumed < g_rx_len) {
        /* Find magic byte */
        if (g_rx_buf[consumed] != CINEMA_PROTO_MAGIC) {
            consumed++;
            continue;
        }

        /* Need at least overhead bytes */
        size_t remaining = g_rx_len - consumed;
        if (remaining < FRAME_OVERHEAD)
            break; /* wait for more data */

        uint8_t cmd = g_rx_buf[consumed + FRAME_OFF_CMD];
        uint8_t len = g_rx_buf[consumed + FRAME_OFF_LEN];
        size_t  total = (size_t)FRAME_OVERHEAD + len;

        if (remaining < total)
            break; /* wait for more data */

        /* Validate CRC (covers cmd + len + payload) */
        uint8_t expected_crc = cinema_crc8(&g_rx_buf[consumed + FRAME_OFF_CMD],
                                            2 + len);
        uint8_t actual_crc   = g_rx_buf[consumed + FRAME_OFF_PAYLOAD + len];

        if (expected_crc != actual_crc) {
            LOGW("drain_tty: CRC mismatch cmd=0x%02x expected=0x%02x got=0x%02x",
                 cmd, expected_crc, actual_crc);
            consumed++; /* skip magic, resync */
            continue;
        }

        /* Dispatch */
        process_frame(cmd,
                      &g_rx_buf[consumed + FRAME_OFF_PAYLOAD],
                      len);

        consumed += total;
    }

    /* Shift unconsumed bytes to front */
    if (consumed > 0 && consumed <= g_rx_len) {
        g_rx_len -= consumed;
        if (g_rx_len > 0)
            memmove(g_rx_buf, g_rx_buf + consumed, g_rx_len);
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Sysfs POLLPRI handler                                                */
/* ------------------------------------------------------------------ */

/*
 * handle_sysfs_pri - service a POLLPRI event on a sysfs fd.
 * Re-reads the value and, if it is nd_setpoint, relays to MCU.
 */
static void handle_sysfs_pri(int fd, const char *path)
{
    char buf[32];
    ssize_t n;
    int32_t val;

    /* POLLPRI on sysfs requires lseek(0) before read */
    if (lseek(fd, 0, SEEK_SET) < 0) {
        LOGE("handle_sysfs_pri: lseek %s: %s", path, strerror(errno));
        return;
    }

    n = read(fd, buf, sizeof(buf) - 1);
    if (n <= 0) {
        LOGE("handle_sysfs_pri: read %s: %s", path, strerror(errno));
        return;
    }
    buf[n] = '\0';

    errno = 0;
    val = (int32_t)strtol(buf, NULL, 10);
    if (errno) {
        LOGE("handle_sysfs_pri: strtol %s: %s", path, strerror(errno));
        return;
    }

    LOGI("sysfs change %s = %" PRId32, path, val);

    /* Only nd_setpoint changes need to be forwarded to the MCU */
    if (fd == g_nd_fd) {
        struct set_nd_payload payload = { .nd_mb = val };
        if (send_frame(CMD_SET_ND, &payload, sizeof(payload)) < 0)
            LOGW("handle_sysfs_pri: failed to send CMD_SET_ND");
        else
            LOGI("CMD_SET_ND sent nd_mb=%" PRId32, val);
    }
}

/* ------------------------------------------------------------------ */
/* MCU connect / disconnect                                             */
/* ------------------------------------------------------------------ */

/*
 * open_cinema_end - open /dev/cinema_end; kernel presence signal.
 * Returns fd on success, -1 on failure.
 */
static int open_cinema_end(void)
{
    int fd = open(DEV_CINEMA_END, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        LOGE("open_cinema_end: %s", strerror(errno));
        return -1;
    }
    LOGI("open_cinema_end: fd=%d", fd);
    return fd;
}

/*
 * start_inotify_watch - watch /dev for new ttyACM* nodes (MCU reconnect).
 */
static void start_inotify_watch(void)
{
    if (g_inotify_wd >= 0)
        return; /* already watching */

    g_inotify_wd = inotify_add_watch(g_inotify_fd, DEV_DIR, IN_CREATE);
    if (g_inotify_wd < 0)
        LOGE("inotify_add_watch /dev: %s", strerror(errno));
    else
        LOGI("inotify: watching /dev for ttyACM* creation");
}

/*
 * stop_inotify_watch - stop watching /dev.
 */
static void stop_inotify_watch(void)
{
    if (g_inotify_wd < 0)
        return;
    inotify_rm_watch(g_inotify_fd, g_inotify_wd);
    g_inotify_wd = -1;
    LOGI("inotify: stopped watching /dev");
}

/*
 * connect_mcu - find MCU tty, open it, open /dev/cinema_end, send heartbeat.
 * Returns 0 on success, -1 on failure.
 */
static int connect_mcu(void)
{
    if (find_mcu_tty() < 0)
        return -1;

    g_tty_fd = open_tty(g_tty_path);
    if (g_tty_fd < 0)
        return -1;

    g_cinema_fd = open_cinema_end();
    if (g_cinema_fd < 0) {
        close(g_tty_fd);
        g_tty_fd = -1;
        return -1;
    }

    if (epoll_add(g_epoll_fd, g_tty_fd, EPOLLIN) < 0) {
        close(g_tty_fd); g_tty_fd = -1;
        close(g_cinema_fd); g_cinema_fd = -1;
        return -1;
    }

    stop_inotify_watch();

    /* Send initial query */
    if (send_frame(CMD_QUERY, NULL, 0) < 0)
        LOGW("connect_mcu: initial CMD_QUERY failed");
    else
        LOGI("connect_mcu: connected, sent CMD_QUERY");

    return 0;
}

/*
 * disconnect_mcu - tear down tty and cinema_end fds, start inotify watch.
 */
static void disconnect_mcu(void)
{
    LOGW("disconnect_mcu: MCU disconnected");

    if (g_tty_fd >= 0) {
        epoll_del(g_epoll_fd, g_tty_fd);
        close(g_tty_fd);
        g_tty_fd = -1;
    }

    /*
     * Closing g_cinema_fd signals the kernel driver to transition to
     * offline/fault state.
     */
    if (g_cinema_fd >= 0) {
        close(g_cinema_fd);
        g_cinema_fd = -1;
    }

    g_rx_len = 0;

    start_inotify_watch();
}

/* ------------------------------------------------------------------ */
/* inotify handler                                                      */
/* ------------------------------------------------------------------ */

static void handle_inotify(void)
{
    char buf[sizeof(struct inotify_event) + NAME_MAX + 1];
    ssize_t n;
    const struct inotify_event *ev;

    n = read(g_inotify_fd, buf, sizeof(buf));
    if (n < 0) {
        if (errno != EAGAIN)
            LOGE("inotify read: %s", strerror(errno));
        return;
    }

    for (char *p = buf; p < buf + n; ) {
        ev = (const struct inotify_event *)p;
        p += sizeof(struct inotify_event) + ev->len;

        if (!(ev->mask & IN_CREATE))
            continue;
        if (!ev->len)
            continue;

        /* Only care about ttyACM* nodes */
        if (strncmp(ev->name, "ttyACM", 6) != 0)
            continue;

        LOGI("inotify: new device /dev/%s — attempting MCU connect", ev->name);

        /* Small settle delay: the USB enumeration may not be complete yet.
         * Use nanosleep for a simple 100 ms wait without threads. */
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 100 * 1000 * 1000L };
        nanosleep(&ts, NULL);

        if (connect_mcu() < 0)
            LOGW("handle_inotify: connect_mcu failed, will retry on next event");
    }
}

/* ------------------------------------------------------------------ */
/* Timer handler (heartbeat)                                            */
/* ------------------------------------------------------------------ */

static void handle_timer(void)
{
    uint64_t expirations;

    /* Drain the timerfd */
    if (read(g_timer_fd, &expirations, sizeof(expirations)) < 0 &&
        errno != EAGAIN)
        LOGE("timer read: %s", strerror(errno));

    if (g_tty_fd < 0)
        return; /* MCU not connected */

    if (send_frame(CMD_QUERY, NULL, 0) < 0) {
        LOGW("heartbeat: CMD_QUERY failed — treating as disconnect");
        disconnect_mcu();
    }
}

/* ------------------------------------------------------------------ */
/* Signal handler                                                       */
/* ------------------------------------------------------------------ */

static void handle_signal(void)
{
    struct signalfd_siginfo si;

    if (read(g_signal_fd, &si, sizeof(si)) != sizeof(si)) {
        LOGE("signalfd read: %s", strerror(errno));
        return;
    }

    LOGI("received signal %u — shutting down", si.ssi_signo);
}

/* ------------------------------------------------------------------ */
/* Initialisation                                                       */
/* ------------------------------------------------------------------ */

static int init_epoll(void)
{
    g_epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (g_epoll_fd < 0) {
        LOGE("epoll_create1: %s", strerror(errno));
        return -1;
    }
    return 0;
}

static int init_signalfd(void)
{
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);

    /* Block signals so they are delivered via signalfd */
    if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0) {
        LOGE("sigprocmask: %s", strerror(errno));
        return -1;
    }

    g_signal_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (g_signal_fd < 0) {
        LOGE("signalfd: %s", strerror(errno));
        return -1;
    }

    return epoll_add(g_epoll_fd, g_signal_fd, EPOLLIN);
}

static int init_inotify(void)
{
    g_inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (g_inotify_fd < 0) {
        LOGE("inotify_init1: %s", strerror(errno));
        return -1;
    }

    return epoll_add(g_epoll_fd, g_inotify_fd, EPOLLIN);
}

static int init_timerfd(void)
{
    struct itimerspec its;

    g_timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (g_timer_fd < 0) {
        LOGE("timerfd_create: %s", strerror(errno));
        return -1;
    }

    its.it_value.tv_sec     = HEARTBEAT_MS / 1000;
    its.it_value.tv_nsec    = (HEARTBEAT_MS % 1000) * 1000000L;
    its.it_interval.tv_sec  = its.it_value.tv_sec;
    its.it_interval.tv_nsec = its.it_value.tv_nsec;

    if (timerfd_settime(g_timer_fd, 0, &its, NULL) < 0) {
        LOGE("timerfd_settime: %s", strerror(errno));
        return -1;
    }

    return epoll_add(g_epoll_fd, g_timer_fd, EPOLLIN);
}

static int init_sysfs_watches(void)
{
    /* Open sysfs files O_RDONLY for POLLPRI-based change notification */
    g_nd_fd = open(SYSFS_ND_SETPOINT, O_RDONLY | O_CLOEXEC);
    if (g_nd_fd < 0) {
        LOGE("open %s: %s", SYSFS_ND_SETPOINT, strerror(errno));
        return -1;
    }

    g_mode_fd = open(SYSFS_MODE, O_RDONLY | O_CLOEXEC);
    if (g_mode_fd < 0) {
        LOGE("open %s: %s", SYSFS_MODE, strerror(errno));
        return -1;
    }

    g_enable_fd = open(SYSFS_ENABLE, O_RDONLY | O_CLOEXEC);
    if (g_enable_fd < 0) {
        LOGE("open %s: %s", SYSFS_ENABLE, strerror(errno));
        return -1;
    }

    if (epoll_add(g_epoll_fd, g_nd_fd,     EPOLLPRI) < 0) return -1;
    if (epoll_add(g_epoll_fd, g_mode_fd,   EPOLLPRI) < 0) return -1;
    if (epoll_add(g_epoll_fd, g_enable_fd, EPOLLPRI) < 0) return -1;

    LOGI("init_sysfs_watches: watching nd_setpoint, mode, enable");
    return 0;
}

/* ------------------------------------------------------------------ */
/* Cleanup                                                              */
/* ------------------------------------------------------------------ */

static void cleanup(void)
{
    LOGI("cleanup: releasing resources");

    if (g_tty_fd >= 0)    { close(g_tty_fd);    g_tty_fd    = -1; }
    if (g_cinema_fd >= 0) { close(g_cinema_fd);  g_cinema_fd = -1; }
    if (g_nd_fd >= 0)     { close(g_nd_fd);      g_nd_fd     = -1; }
    if (g_mode_fd >= 0)   { close(g_mode_fd);    g_mode_fd   = -1; }
    if (g_enable_fd >= 0) { close(g_enable_fd);  g_enable_fd = -1; }
    if (g_timer_fd >= 0)  { close(g_timer_fd);   g_timer_fd  = -1; }
    if (g_inotify_fd >= 0){ close(g_inotify_fd); g_inotify_fd= -1; }
    if (g_signal_fd >= 0) { close(g_signal_fd);  g_signal_fd = -1; }
    if (g_epoll_fd >= 0)  { close(g_epoll_fd);   g_epoll_fd  = -1; }
}

/* ------------------------------------------------------------------ */
/* Main event loop                                                      */
/* ------------------------------------------------------------------ */

int main(void)
{
    struct epoll_event events[EPOLL_MAX_EVENTS];
    int nfds;
    int running = 1;

    LOGI("cinema_end_daemon starting");

    if (init_epoll() < 0)        goto fail;
    if (init_signalfd() < 0)     goto fail;
    if (init_inotify() < 0)      goto fail;
    if (init_timerfd() < 0)      goto fail;
    if (init_sysfs_watches() < 0) goto fail;

    /* Attempt initial MCU connection */
    if (connect_mcu() < 0) {
        LOGW("main: MCU not present at startup, waiting for USB connect");
        start_inotify_watch();
    }

    LOGI("main: entering event loop");

    while (running) {
        nfds = epoll_wait(g_epoll_fd, events, EPOLL_MAX_EVENTS, -1);
        if (nfds < 0) {
            if (errno == EINTR)
                continue;
            LOGE("epoll_wait: %s", strerror(errno));
            break;
        }

        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;

            if (fd == g_signal_fd) {
                handle_signal();
                running = 0;
                break;

            } else if (fd == g_timer_fd) {
                handle_timer();

            } else if (fd == g_inotify_fd) {
                handle_inotify();

            } else if (fd == g_tty_fd) {
                if (drain_tty() < 0) {
                    LOGE("main: tty I/O error — disconnecting MCU");
                    disconnect_mcu();
                }

            } else if (fd == g_nd_fd) {
                handle_sysfs_pri(g_nd_fd, SYSFS_ND_SETPOINT);

            } else if (fd == g_mode_fd) {
                handle_sysfs_pri(g_mode_fd, SYSFS_MODE);

            } else if (fd == g_enable_fd) {
                handle_sysfs_pri(g_enable_fd, SYSFS_ENABLE);

            } else {
                LOGW("main: unhandled fd=%d in epoll", fd);
            }
        }
    }

    LOGI("main: event loop exited cleanly");
    cleanup();
    return 0;

fail:
    LOGE("main: fatal initialisation error");
    cleanup();
    return 1;
}
