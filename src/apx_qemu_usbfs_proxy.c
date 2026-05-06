/*
 * APX usbfs relay client for QEMU linux-user.
 *
 * This file is intended to be included from linux-user/syscall.c in the
 * qemu-i386-usbfix tree. It turns selected Linux USBFS file opens/ioctls into
 * APXR TCP RPCs to the macOS apx-relayd daemon.
 *
 * Runtime:
 *   APX_RELAY_HOST=host.docker.internal:17523
 *   APX_FAKE_USB_PATH=/dev/bus/usb/001/001
 */

#ifndef APX_QEMU_USBFS_PROXY_C
#define APX_QEMU_USBFS_PROXY_C

#if !defined(CONFIG_USBFS)
#error "apx_qemu_usbfs_proxy.c requires CONFIG_USBFS"
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define APX_USBFS_PROXY_NOT_HANDLED (-2)
#define APX_USBFS_PROXY_MAX_TRANSFER (16u * 1024u * 1024u)
#define APX_USBFS_PROXY_ENV "APX_RELAY_HOST"
#define APX_USBFS_PROXY_ENV_ALT "APX_RELAY_ADDR"
#define APX_USBFS_PROXY_DEFAULT_PORT "17523"
#define APX_USBFS_PROXY_IOCTL_NOT_HANDLED (-3)
#define APX_USBFS_PROXY_IOC_CONTROL_RAW 0xc0105500u
#define APX_USBFS_PROXY_IOC_BULK_RAW 0xc0105502u
#define APX_USBFS_PROXY_IOC_CLAIMINTERFACE_RAW 0x8004550f
#define APX_USBFS_PROXY_IOC_RELEASEINTERFACE_RAW 0x80045510

#define APX_USBFS_PROXY_PATH_DEV "/dev/bus/usb/"
#define APX_USBFS_PROXY_PATH_PROC "/proc/bus/usb/"

#define APXR_MAGIC 0x41505852u
#define APXR_PROTOCOL_VERSION 1u
#define APXR_HEADER_SIZE 20u
#define APXR_NVIDIA_VID 0x0955u
#define APXR_APX_PID 0x7523u

#define APXR_OP_OPEN 2u
#define APXR_OP_CLAIM_INTERFACE 3u
#define APXR_OP_RELEASE_INTERFACE 4u
#define APXR_OP_CONTROL_TRANSFER 5u
#define APXR_OP_BULK_READ 6u
#define APXR_OP_BULK_WRITE 7u
#define APXR_OP_RESET_DEVICE 8u
#define APXR_OP_CLOSE 9u

static GMutex apx_usbfs_proxy_fds_lock;
static GHashTable *apx_usbfs_proxy_fds;
static GHashTable *apx_usbfs_proxy_read_offsets;
static GMutex apx_usbfs_proxy_seq_lock;
static uint32_t apx_usbfs_proxy_seq;

static uint16_t apx_read_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint32_t apx_read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

static uint16_t apx_read_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t apx_read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int32_t apx_read_be_i32(const uint8_t *p)
{
    return (int32_t)apx_read_be32(p);
}

static void apx_write_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void apx_write_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t apx_usbfs_proxy_next_seq(void)
{
    uint32_t seq;

    g_mutex_lock(&apx_usbfs_proxy_seq_lock);
    seq = ++apx_usbfs_proxy_seq;
    if (seq == 0) {
        seq = ++apx_usbfs_proxy_seq;
    }
    g_mutex_unlock(&apx_usbfs_proxy_seq_lock);

    return seq;
}

static void apx_usbfs_proxy_init_fds_locked(void)
{
    if (!apx_usbfs_proxy_fds) {
        apx_usbfs_proxy_fds = g_hash_table_new(g_direct_hash, g_direct_equal);
    }
    if (!apx_usbfs_proxy_read_offsets) {
        apx_usbfs_proxy_read_offsets = g_hash_table_new(g_direct_hash,
                                                        g_direct_equal);
    }
}

static void apx_usbfs_proxy_mark_fd(int fd)
{
    g_mutex_lock(&apx_usbfs_proxy_fds_lock);
    apx_usbfs_proxy_init_fds_locked();
    g_hash_table_add(apx_usbfs_proxy_fds, GINT_TO_POINTER(fd));
    g_hash_table_insert(apx_usbfs_proxy_read_offsets, GINT_TO_POINTER(fd),
                        GSIZE_TO_POINTER(0));
    g_mutex_unlock(&apx_usbfs_proxy_fds_lock);
}

static void apx_usbfs_proxy_unmark_fd(int fd)
{
    g_mutex_lock(&apx_usbfs_proxy_fds_lock);
    if (apx_usbfs_proxy_fds) {
        g_hash_table_remove(apx_usbfs_proxy_fds, GINT_TO_POINTER(fd));
    }
    if (apx_usbfs_proxy_read_offsets) {
        g_hash_table_remove(apx_usbfs_proxy_read_offsets, GINT_TO_POINTER(fd));
    }
    g_mutex_unlock(&apx_usbfs_proxy_fds_lock);
}

static size_t apx_usbfs_proxy_get_read_offset(int fd)
{
    gpointer value;

    g_mutex_lock(&apx_usbfs_proxy_fds_lock);
    value = apx_usbfs_proxy_read_offsets ?
            g_hash_table_lookup(apx_usbfs_proxy_read_offsets,
                                GINT_TO_POINTER(fd)) : NULL;
    g_mutex_unlock(&apx_usbfs_proxy_fds_lock);

    return (size_t)GPOINTER_TO_SIZE(value);
}

static void apx_usbfs_proxy_set_read_offset(int fd, size_t offset)
{
    g_mutex_lock(&apx_usbfs_proxy_fds_lock);
    apx_usbfs_proxy_init_fds_locked();
    g_hash_table_insert(apx_usbfs_proxy_read_offsets, GINT_TO_POINTER(fd),
                        GSIZE_TO_POINTER(offset));
    g_mutex_unlock(&apx_usbfs_proxy_fds_lock);
}

static bool apx_qemu_usbfs_proxy_is_fd(int fd)
{
    bool found;

    if (fd < 0) {
        return false;
    }

    g_mutex_lock(&apx_usbfs_proxy_fds_lock);
    found = apx_usbfs_proxy_fds &&
            g_hash_table_contains(apx_usbfs_proxy_fds, GINT_TO_POINTER(fd));
    g_mutex_unlock(&apx_usbfs_proxy_fds_lock);

    return found;
}

static const char *apx_usbfs_proxy_relay_addr(void)
{
    const char *addr = getenv(APX_USBFS_PROXY_ENV);

    if (!addr || !*addr) {
        addr = getenv(APX_USBFS_PROXY_ENV_ALT);
    }
    if (!addr || !*addr) {
        return NULL;
    }
    return addr;
}

static bool apx_usbfs_proxy_parse_prefixed_path(const char *path,
                                                const char *prefix,
                                                uint32_t *bus,
                                                uint32_t *dev)
{
    const char *p;
    char *end;
    unsigned long parsed_bus;
    unsigned long parsed_dev;
    size_t prefix_len = strlen(prefix);

    if (strncmp(path, prefix, prefix_len) != 0) {
        return false;
    }

    p = path + prefix_len;
    errno = 0;
    parsed_bus = strtoul(p, &end, 10);
    if (errno || end == p || *end != '/') {
        return false;
    }

    p = end + 1;
    errno = 0;
    parsed_dev = strtoul(p, &end, 10);
    if (errno || end == p || *end != '\0') {
        return false;
    }

    if (parsed_bus > UINT32_MAX || parsed_dev > UINT32_MAX) {
        return false;
    }

    *bus = (uint32_t)parsed_bus;
    *dev = (uint32_t)parsed_dev;
    return true;
}

static bool apx_usbfs_proxy_parse_path(const char *path,
                                       uint32_t *bus,
                                       uint32_t *dev)
{
    return apx_usbfs_proxy_parse_prefixed_path(path, APX_USBFS_PROXY_PATH_DEV,
                                               bus, dev) ||
           apx_usbfs_proxy_parse_prefixed_path(path, APX_USBFS_PROXY_PATH_PROC,
                                               bus, dev);
}

static int apx_usbfs_proxy_status_errno(int status)
{
    switch (status) {
    case -1:  return EIO;       /* LIBUSB_ERROR_IO */
    case -2:  return EINVAL;    /* LIBUSB_ERROR_INVALID_PARAM */
    case -3:  return EACCES;    /* LIBUSB_ERROR_ACCESS */
    case -4:  return ENODEV;    /* LIBUSB_ERROR_NO_DEVICE */
    case -5:  return ENOENT;    /* LIBUSB_ERROR_NOT_FOUND */
    case -6:  return EBUSY;     /* LIBUSB_ERROR_BUSY */
    case -7:  return ETIMEDOUT; /* LIBUSB_ERROR_TIMEOUT */
    case -8:  return EOVERFLOW; /* LIBUSB_ERROR_OVERFLOW */
    case -9:  return EPIPE;     /* LIBUSB_ERROR_PIPE */
    case -10: return EINTR;     /* LIBUSB_ERROR_INTERRUPTED */
    case -11: return ENOMEM;    /* LIBUSB_ERROR_NO_MEM */
    case -12: return ENOSYS;    /* LIBUSB_ERROR_NOT_SUPPORTED */
    default:  return EIO;
    }
}

static abi_long apx_usbfs_proxy_target_errno(int host_errno)
{
    if (host_errno <= 0) {
        host_errno = EIO;
    }
    errno = host_errno;
    return get_errno(-1);
}

static ssize_t apx_usbfs_proxy_write_some(int fd, const void *buf, size_t len)
{
#ifdef MSG_NOSIGNAL
    return send(fd, buf, len, MSG_NOSIGNAL);
#else
    return write(fd, buf, len);
#endif
}

static int apx_usbfs_proxy_write_exact(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;

    while (len) {
        ssize_t n = apx_usbfs_proxy_write_some(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            errno = EPIPE;
            return -1;
        }
        p += n;
        len -= n;
    }

    return 0;
}

static int apx_usbfs_proxy_read_exact(int fd, void *buf, size_t len)
{
    uint8_t *p = buf;

    while (len) {
        ssize_t n = read(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            errno = ECONNRESET;
            return -1;
        }
        p += n;
        len -= n;
    }

    return 0;
}

static void apx_usbfs_proxy_drain(int fd, size_t len)
{
    uint8_t discard[4096];

    while (len) {
        size_t chunk = MIN(len, sizeof(discard));
        if (apx_usbfs_proxy_read_exact(fd, discard, chunk) < 0) {
            return;
        }
        len -= chunk;
    }
}

static int apx_usbfs_proxy_parse_host_port(const char *addr,
                                           char *host,
                                           size_t host_len,
                                           char *port,
                                           size_t port_len)
{
    const char *colon;
    size_t n;

    if (!addr || !*addr) {
        errno = EINVAL;
        return -1;
    }

    colon = strrchr(addr, ':');
    if (!colon || colon == addr || colon[1] == '\0') {
        if (strlen(addr) >= host_len || strlen(APX_USBFS_PROXY_DEFAULT_PORT) >= port_len) {
            errno = ENAMETOOLONG;
            return -1;
        }
        strcpy(host, addr);
        strcpy(port, APX_USBFS_PROXY_DEFAULT_PORT);
        return 0;
    }

    n = (size_t)(colon - addr);
    if (n >= host_len || strlen(colon + 1) >= port_len) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(host, addr, n);
    host[n] = '\0';
    strcpy(port, colon + 1);
    return 0;
}

static int apx_usbfs_proxy_connect(void)
{
    const char *relay_addr = apx_usbfs_proxy_relay_addr();
    char host[256];
    char port[32];
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    struct addrinfo *rp;
    int fd = -1;
    int gai;

    if (!relay_addr) {
        return APX_USBFS_PROXY_NOT_HANDLED;
    }
    if (apx_usbfs_proxy_parse_host_port(relay_addr, host, sizeof(host),
                                        port, sizeof(port)) < 0) {
        return -1;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;

    gai = getaddrinfo(host, port, &hints, &result);
    if (gai != 0) {
        errno = EHOSTUNREACH;
        return -1;
    }

    for (rp = result; rp; rp = rp->ai_next) {
#ifdef SOCK_CLOEXEC
        fd = socket(rp->ai_family, rp->ai_socktype | SOCK_CLOEXEC,
                    rp->ai_protocol);
        if (fd < 0 && errno == EINVAL) {
            fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        }
#else
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
#endif
        if (fd < 0) {
            continue;
        }
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }

    freeaddrinfo(result);
    if (fd < 0) {
        errno = ECONNREFUSED;
    }
    return fd;
}

static int apx_usbfs_proxy_rpc(int fd,
                               uint16_t opcode,
                               uint32_t seq,
                               const void *out_payload,
                               size_t out_len,
                               void *in_payload,
                               size_t in_cap,
                               size_t *actual_len)
{
    uint8_t prefix[4];
    uint8_t header[APXR_HEADER_SIZE];
    uint32_t frame_len;
    uint32_t response_frame_len;
    uint32_t magic;
    uint16_t version;
    uint16_t response_opcode;
    uint32_t response_seq;
    int32_t status;
    uint32_t payload_len;

    if (out_len > APX_USBFS_PROXY_MAX_TRANSFER) {
        errno = EMSGSIZE;
        return -1;
    }

    frame_len = APXR_HEADER_SIZE + (uint32_t)out_len;
    apx_write_be32(prefix, frame_len);
    apx_write_be32(header + 0, APXR_MAGIC);
    apx_write_be16(header + 4, APXR_PROTOCOL_VERSION);
    apx_write_be16(header + 6, opcode);
    apx_write_be32(header + 8, seq);
    apx_write_be32(header + 12, 0);
    apx_write_be32(header + 16, (uint32_t)out_len);

    if (apx_usbfs_proxy_write_exact(fd, prefix, sizeof(prefix)) < 0 ||
        apx_usbfs_proxy_write_exact(fd, header, sizeof(header)) < 0 ||
        (out_len && apx_usbfs_proxy_write_exact(fd, out_payload, out_len) < 0)) {
        return -1;
    }

    if (apx_usbfs_proxy_read_exact(fd, prefix, sizeof(prefix)) < 0) {
        return -1;
    }
    response_frame_len = apx_read_be32(prefix);
    if (response_frame_len < APXR_HEADER_SIZE ||
        response_frame_len > APX_USBFS_PROXY_MAX_TRANSFER + APXR_HEADER_SIZE) {
        errno = EMSGSIZE;
        return -1;
    }
    if (apx_usbfs_proxy_read_exact(fd, header, sizeof(header)) < 0) {
        return -1;
    }

    magic = apx_read_be32(header + 0);
    version = apx_read_be16(header + 4);
    response_opcode = apx_read_be16(header + 6);
    response_seq = apx_read_be32(header + 8);
    status = apx_read_be_i32(header + 12);
    payload_len = apx_read_be32(header + 16);

    if (magic != APXR_MAGIC || version != APXR_PROTOCOL_VERSION ||
        response_opcode != opcode || response_seq != seq ||
        payload_len != response_frame_len - APXR_HEADER_SIZE) {
        apx_usbfs_proxy_drain(fd, response_frame_len - APXR_HEADER_SIZE);
        errno = EPROTO;
        return -1;
    }

    if (payload_len > in_cap || (payload_len && !in_payload)) {
        apx_usbfs_proxy_drain(fd, payload_len);
        errno = EOVERFLOW;
        return -1;
    }
    if (payload_len && apx_usbfs_proxy_read_exact(fd, in_payload, payload_len) < 0) {
        return -1;
    }
    if (actual_len) {
        *actual_len = payload_len;
    }

    if (status < 0) {
        errno = apx_usbfs_proxy_status_errno(status);
        return -1;
    }
    return status;
}

static int apx_usbfs_proxy_simple_rpc(int fd, uint16_t opcode,
                                      const void *payload, size_t payload_len,
                                      void *response, size_t response_cap,
                                      size_t *actual_len)
{
    return apx_usbfs_proxy_rpc(fd, opcode, apx_usbfs_proxy_next_seq(),
                               payload, payload_len, response, response_cap,
                               actual_len);
}

static int apx_usbfs_proxy_control_get_descriptor(int fd,
                                                  uint16_t value,
                                                  uint16_t index,
                                                  uint16_t length,
                                                  uint8_t *out,
                                                  size_t out_cap,
                                                  size_t *actual_len)
{
    uint8_t payload[12];

    if (length > out_cap) {
        errno = EMSGSIZE;
        return -1;
    }

    memset(payload, 0, sizeof(payload));
    payload[0] = USB_DIR_IN;
    payload[1] = 6; /* GET_DESCRIPTOR */
    apx_write_be16(payload + 2, value);
    apx_write_be16(payload + 4, index);
    apx_write_be16(payload + 6, length);
    apx_write_be32(payload + 8, 5000);

    return apx_usbfs_proxy_simple_rpc(fd, APXR_OP_CONTROL_TRANSFER,
                                      payload, sizeof(payload),
                                      out, out_cap, actual_len);
}

static ssize_t apx_usbfs_proxy_load_descriptor_blob(int fd,
                                                    uint8_t *blob,
                                                    size_t blob_cap)
{
    uint8_t config_head[9];
    size_t actual = 0;
    size_t pos = 0;
    uint16_t total_len;
    int ret;

    ret = apx_usbfs_proxy_control_get_descriptor(fd, 0x0100, 0, 18,
                                                 blob, blob_cap, &actual);
    if (ret < 0) {
        return -1;
    }
    pos += actual;

    ret = apx_usbfs_proxy_control_get_descriptor(fd, 0x0200, 0,
                                                 sizeof(config_head),
                                                 config_head,
                                                 sizeof(config_head),
                                                 &actual);
    if (ret < 0) {
        return -1;
    }
    if (actual < sizeof(config_head)) {
        errno = EPROTO;
        return -1;
    }

    total_len = (uint16_t)config_head[2] | ((uint16_t)config_head[3] << 8);
    if (total_len < sizeof(config_head) || pos + total_len > blob_cap) {
        errno = EOVERFLOW;
        return -1;
    }

    ret = apx_usbfs_proxy_control_get_descriptor(fd, 0x0200, 0,
                                                 total_len,
                                                 blob + pos,
                                                 blob_cap - pos,
                                                 &actual);
    if (ret < 0) {
        return -1;
    }
    pos += actual;

    return (ssize_t)pos;
}

static abi_long apx_qemu_usbfs_proxy_read(int fd, void *buf, size_t count)
{
    uint8_t blob[4096];
    ssize_t blob_len;
    size_t offset;
    size_t n;

    if (count == 0) {
        return 0;
    }

    blob_len = apx_usbfs_proxy_load_descriptor_blob(fd, blob, sizeof(blob));
    if (blob_len < 0) {
        return apx_usbfs_proxy_target_errno(errno);
    }

    offset = apx_usbfs_proxy_get_read_offset(fd);
    if (offset >= (size_t)blob_len) {
        return 0;
    }

    n = MIN(count, (size_t)blob_len - offset);
    memcpy(buf, blob + offset, n);
    apx_usbfs_proxy_set_read_offset(fd, offset + n);
    return (abi_long)n;
}

static void apx_usbfs_proxy_send_close(int fd)
{
    (void)apx_usbfs_proxy_simple_rpc(fd, APXR_OP_CLOSE, NULL, 0, NULL, 0, NULL);
}

static int apx_qemu_usbfs_proxy_open(const char *pathname, int flags)
{
    uint8_t payload[12];
    uint8_t response[8];
    uint32_t bus;
    uint32_t dev;
    int fd;
    int saved_errno;

    if (!apx_usbfs_proxy_parse_path(pathname, &bus, &dev)) {
        return APX_USBFS_PROXY_NOT_HANDLED;
    }

    fd = apx_usbfs_proxy_connect();
    if (fd == APX_USBFS_PROXY_NOT_HANDLED) {
        return APX_USBFS_PROXY_NOT_HANDLED;
    }
    if (fd < 0) {
        return -1;
    }

    memset(payload, 0, sizeof(payload));
    apx_write_be16(payload + 0, APXR_NVIDIA_VID);
    apx_write_be16(payload + 2, APXR_APX_PID);
    apx_write_be32(payload + 4, 0); /* ordinal */
    payload[8] = 0;                 /* bus wildcard: macOS bus differs */
    payload[9] = 0;                 /* address wildcard */

    if (apx_usbfs_proxy_simple_rpc(fd, APXR_OP_OPEN, payload, sizeof(payload),
                                   response, sizeof(response), NULL) < 0) {
        saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }

    if (flags & O_CLOEXEC) {
        (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    } else {
        (void)fcntl(fd, F_SETFD, 0);
    }

    apx_usbfs_proxy_mark_fd(fd);
    return fd;
}

static abi_long apx_qemu_usbfs_proxy_close(int fd)
{
    apx_usbfs_proxy_send_close(fd);
    apx_usbfs_proxy_unmark_fd(fd);
    return get_errno(close(fd));
}

static abi_long apx_usbfs_proxy_ioctl_int_arg(const IOCTLEntry *ie,
                                              uint8_t *buf_temp,
                                              int fd,
                                              abi_long arg,
                                              uint16_t opcode)
{
    uint8_t payload[4];
    const argtype *arg_type = ie->arg_type + 1;
    int target_size = thunk_type_size(arg_type, THUNK_TARGET);
    int value;
    void *argptr;
    int ret;

    argptr = lock_user(VERIFY_READ, arg, target_size, 1);
    if (!argptr) {
        return -TARGET_EFAULT;
    }

    thunk_convert(buf_temp, argptr, arg_type, THUNK_HOST);
    unlock_user(argptr, arg, 0);
    value = *(int *)buf_temp;

    memset(payload, 0, sizeof(payload));
    payload[0] = (uint8_t)value;

    ret = apx_usbfs_proxy_simple_rpc(fd, opcode, payload, sizeof(payload),
                                     NULL, 0, NULL);
    if (ret < 0) {
        return apx_usbfs_proxy_target_errno(errno);
    }

    return ret;
}

static abi_long apx_usbfs_proxy_ioctl_reset(int fd)
{
    int ret;

    ret = apx_usbfs_proxy_simple_rpc(fd, APXR_OP_RESET_DEVICE, NULL, 0,
                                     NULL, 0, NULL);
    if (ret < 0) {
        return apx_usbfs_proxy_target_errno(errno);
    }

    return ret;
}

static abi_long apx_usbfs_proxy_ioctl_interface_value(int fd,
                                                      abi_long arg,
                                                      uint16_t opcode)
{
    uint8_t payload[4];
    int ret;

    memset(payload, 0, sizeof(payload));
    payload[0] = (uint8_t)arg;

    ret = apx_usbfs_proxy_simple_rpc(fd, opcode, payload, sizeof(payload),
                                     NULL, 0, NULL);
    if (ret < 0) {
        return apx_usbfs_proxy_target_errno(errno);
    }

    return ret;
}

static abi_long apx_usbfs_proxy_ioctl_interface_pointer(int fd,
                                                        abi_long arg,
                                                        uint16_t opcode)
{
    uint32_t value;
    void *argptr;

    argptr = lock_user(VERIFY_READ, arg, sizeof(value), 1);
    if (!argptr) {
        return -TARGET_EFAULT;
    }
    memcpy(&value, argptr, sizeof(value));
    unlock_user(argptr, arg, 0);

    return apx_usbfs_proxy_ioctl_interface_value(fd, (abi_long)value, opcode);
}

static abi_long apx_usbfs_proxy_ioctl_control_raw(int fd, abi_long arg)
{
    uint8_t local[16];
    uint8_t *payload = NULL;
    uint8_t request_type;
    uint8_t request;
    uint16_t value;
    uint16_t index;
    uint16_t length;
    uint32_t timeout;
    uint32_t target_data;
    void *argptr;
    void *data = NULL;
    size_t actual = 0;
    bool is_in;
    abi_long ret;

    argptr = lock_user(VERIFY_READ, arg, sizeof(local), 1);
    if (!argptr) {
        return -TARGET_EFAULT;
    }
    memcpy(local, argptr, sizeof(local));
    unlock_user(argptr, arg, 0);

    request_type = local[0];
    request = local[1];
    value = apx_read_le16(local + 2);
    index = apx_read_le16(local + 4);
    length = apx_read_le16(local + 6);
    timeout = apx_read_le32(local + 8);
    target_data = apx_read_le32(local + 12);
    is_in = (request_type & USB_DIR_IN) != 0;

    if (length && target_data) {
        int rw_dir = is_in ? VERIFY_WRITE : VERIFY_READ;
        data = lock_user(rw_dir, target_data, length, rw_dir == VERIFY_READ);
        if (!data) {
            return -TARGET_EFAULT;
        }
    }

    payload = malloc(12u + (is_in ? 0u : length));
    if (!payload) {
        if (data) {
            unlock_user(data, target_data, 0);
        }
        return -TARGET_ENOMEM;
    }
    payload[0] = request_type;
    payload[1] = request;
    apx_write_be16(payload + 2, value);
    apx_write_be16(payload + 4, index);
    apx_write_be16(payload + 6, length);
    apx_write_be32(payload + 8, timeout);
    if (!is_in && length) {
        memcpy(payload + 12, data, length);
    }

    ret = apx_usbfs_proxy_simple_rpc(fd, APXR_OP_CONTROL_TRANSFER,
                                     payload, 12u + (is_in ? 0u : length),
                                     is_in ? data : NULL,
                                     is_in ? length : 0,
                                     &actual);
    if (ret < 0) {
        ret = apx_usbfs_proxy_target_errno(errno);
    }

    free(payload);
    if (data) {
        unlock_user(data, target_data,
                    (is_in && !is_error(ret)) ? MIN(actual, length) : 0);
    }
    return ret;
}

static abi_long apx_usbfs_proxy_ioctl_bulk_raw(int fd, abi_long arg)
{
    uint8_t local[16];
    uint8_t *payload = NULL;
    uint32_t endpoint;
    uint32_t length;
    uint32_t timeout;
    uint32_t target_data;
    void *argptr;
    void *data = NULL;
    size_t actual = 0;
    bool is_in;
    abi_long ret;
    size_t payload_len;
    uint16_t opcode;

    argptr = lock_user(VERIFY_READ, arg, sizeof(local), 1);
    if (!argptr) {
        return -TARGET_EFAULT;
    }
    memcpy(local, argptr, sizeof(local));
    unlock_user(argptr, arg, 0);

    endpoint = apx_read_le32(local + 0);
    length = apx_read_le32(local + 4);
    timeout = apx_read_le32(local + 8);
    target_data = apx_read_le32(local + 12);
    is_in = (endpoint & USB_DIR_IN) != 0;

    if (length > APX_USBFS_PROXY_MAX_TRANSFER) {
        return -TARGET_EMSGSIZE;
    }
    if (length && target_data) {
        int rw_dir = is_in ? VERIFY_WRITE : VERIFY_READ;
        data = lock_user(rw_dir, target_data, length, rw_dir == VERIFY_READ);
        if (!data) {
            return -TARGET_EFAULT;
        }
    }

    if (is_in) {
        payload_len = 12;
        opcode = APXR_OP_BULK_READ;
    } else {
        payload_len = 8u + length;
        opcode = APXR_OP_BULK_WRITE;
    }
    payload = malloc(payload_len);
    if (!payload) {
        if (data) {
            unlock_user(data, target_data, 0);
        }
        return -TARGET_ENOMEM;
    }
    memset(payload, 0, payload_len);
    payload[0] = (uint8_t)endpoint;
    if (is_in) {
        apx_write_be32(payload + 4, length);
        apx_write_be32(payload + 8, timeout);
    } else {
        apx_write_be32(payload + 4, timeout);
        if (length) {
            memcpy(payload + 8, data, length);
        }
    }

    ret = apx_usbfs_proxy_simple_rpc(fd, opcode, payload, payload_len,
                                     is_in ? data : NULL,
                                     is_in ? length : 0,
                                     &actual);
    if (ret < 0) {
        ret = apx_usbfs_proxy_target_errno(errno);
    }

    free(payload);
    if (data) {
        unlock_user(data, target_data,
                    (is_in && !is_error(ret)) ? MIN(actual, length) : 0);
    }
    return ret;
}

static abi_long apx_qemu_usbfs_proxy_do_ioctl_direct(int fd,
                                                     int cmd,
                                                     abi_long arg)
{
    if ((uint32_t)cmd == APX_USBFS_PROXY_IOC_CONTROL_RAW) {
        return apx_usbfs_proxy_ioctl_control_raw(fd, arg);
    }
    if ((uint32_t)cmd == APX_USBFS_PROXY_IOC_BULK_RAW) {
        return apx_usbfs_proxy_ioctl_bulk_raw(fd, arg);
    }
    if ((uint32_t)cmd == APX_USBFS_PROXY_IOC_CLAIMINTERFACE_RAW) {
        return apx_usbfs_proxy_ioctl_interface_pointer(fd, arg,
                                                       APXR_OP_CLAIM_INTERFACE);
    }
    if ((uint32_t)cmd == APX_USBFS_PROXY_IOC_RELEASEINTERFACE_RAW) {
        return apx_usbfs_proxy_ioctl_interface_pointer(fd, arg,
                                                       APXR_OP_RELEASE_INTERFACE);
    }

    switch (cmd) {
    case TARGET_USBDEVFS_CLAIMINTERFACE:
        return apx_usbfs_proxy_ioctl_interface_value(fd, arg,
                                                     APXR_OP_CLAIM_INTERFACE);
    case TARGET_USBDEVFS_RELEASEINTERFACE:
        return apx_usbfs_proxy_ioctl_interface_value(fd, arg,
                                                     APXR_OP_RELEASE_INTERFACE);
    case TARGET_USBDEVFS_RESET:
        return apx_usbfs_proxy_ioctl_reset(fd);
    default:
        return APX_USBFS_PROXY_IOCTL_NOT_HANDLED;
    }
}

static abi_long apx_usbfs_proxy_ioctl_control(const IOCTLEntry *ie,
                                              uint8_t *buf_temp,
                                              int fd,
                                              abi_long arg)
{
    const argtype *arg_type = ie->arg_type + 1;
    struct usbdevfs_ctrltransfer *ctrl;
    abi_ulong target_data;
    uint8_t *payload = NULL;
    void *argptr;
    void *data = NULL;
    size_t actual = 0;
    int target_size;
    int rw_dir;
    bool is_in;
    abi_long ret;

    target_size = thunk_type_size(arg_type, THUNK_TARGET);
    argptr = lock_user(VERIFY_READ, arg, target_size, 1);
    if (!argptr) {
        return -TARGET_EFAULT;
    }

    thunk_convert(buf_temp, argptr, arg_type, THUNK_HOST);
    unlock_user(argptr, arg, 0);

    ctrl = (struct usbdevfs_ctrltransfer *)buf_temp;
    target_data = (abi_ulong)(uintptr_t)ctrl->data;
    is_in = (ctrl->bRequestType & USB_DIR_IN) != 0;

    if (ctrl->wLength && target_data) {
        rw_dir = is_in ? VERIFY_WRITE : VERIFY_READ;
        data = lock_user(rw_dir, target_data, ctrl->wLength,
                         rw_dir == VERIFY_READ);
        if (!data) {
            return -TARGET_EFAULT;
        }
    }

    payload = malloc(12u + (is_in ? 0u : ctrl->wLength));
    if (!payload) {
        if (data) {
            unlock_user(data, target_data, 0);
        }
        return -TARGET_ENOMEM;
    }
    payload[0] = ctrl->bRequestType;
    payload[1] = ctrl->bRequest;
    apx_write_be16(payload + 2, ctrl->wValue);
    apx_write_be16(payload + 4, ctrl->wIndex);
    apx_write_be16(payload + 6, ctrl->wLength);
    apx_write_be32(payload + 8, ctrl->timeout);
    if (!is_in && ctrl->wLength) {
        memcpy(payload + 12, data, ctrl->wLength);
    }

    ret = apx_usbfs_proxy_simple_rpc(fd, APXR_OP_CONTROL_TRANSFER,
                                     payload, 12u + (is_in ? 0u : ctrl->wLength),
                                     is_in ? data : NULL,
                                     is_in ? ctrl->wLength : 0,
                                     &actual);
    if (ret < 0) {
        ret = apx_usbfs_proxy_target_errno(errno);
    }

    free(payload);
    if (data) {
        unlock_user(data, target_data,
                    (is_in && !is_error(ret)) ? MIN(actual, ctrl->wLength) : 0);
    }

    return ret;
}

static abi_long apx_usbfs_proxy_ioctl_bulk(const IOCTLEntry *ie,
                                           uint8_t *buf_temp,
                                           int fd,
                                           abi_long arg)
{
    const argtype *arg_type = ie->arg_type + 1;
    struct usbdevfs_bulktransfer *bulk;
    abi_ulong target_data;
    uint8_t *payload = NULL;
    void *argptr;
    void *data = NULL;
    size_t actual = 0;
    int target_size;
    int rw_dir;
    bool is_in;
    abi_long ret;
    size_t payload_len;
    uint16_t opcode;

    target_size = thunk_type_size(arg_type, THUNK_TARGET);
    argptr = lock_user(VERIFY_READ, arg, target_size, 1);
    if (!argptr) {
        return -TARGET_EFAULT;
    }

    thunk_convert(buf_temp, argptr, arg_type, THUNK_HOST);
    unlock_user(argptr, arg, 0);

    bulk = (struct usbdevfs_bulktransfer *)buf_temp;
    if (bulk->len > APX_USBFS_PROXY_MAX_TRANSFER) {
        return -TARGET_EMSGSIZE;
    }

    target_data = (abi_ulong)(uintptr_t)bulk->data;
    is_in = (bulk->ep & USB_DIR_IN) != 0;

    if (bulk->len && target_data) {
        rw_dir = is_in ? VERIFY_WRITE : VERIFY_READ;
        data = lock_user(rw_dir, target_data, bulk->len,
                         rw_dir == VERIFY_READ);
        if (!data) {
            return -TARGET_EFAULT;
        }
    }

    if (is_in) {
        payload_len = 12;
        opcode = APXR_OP_BULK_READ;
    } else {
        payload_len = 8u + bulk->len;
        opcode = APXR_OP_BULK_WRITE;
    }

    payload = malloc(payload_len);
    if (!payload) {
        if (data) {
            unlock_user(data, target_data, 0);
        }
        return -TARGET_ENOMEM;
    }
    memset(payload, 0, payload_len);
    payload[0] = (uint8_t)bulk->ep;
    if (is_in) {
        apx_write_be32(payload + 4, bulk->len);
        apx_write_be32(payload + 8, bulk->timeout);
    } else {
        apx_write_be32(payload + 4, bulk->timeout);
        if (bulk->len) {
            memcpy(payload + 8, data, bulk->len);
        }
    }

    ret = apx_usbfs_proxy_simple_rpc(fd, opcode, payload, payload_len,
                                     is_in ? data : NULL,
                                     is_in ? bulk->len : 0,
                                     &actual);
    if (ret < 0) {
        ret = apx_usbfs_proxy_target_errno(errno);
    }

    free(payload);
    if (data) {
        unlock_user(data, target_data,
                    (is_in && !is_error(ret)) ? MIN(actual, bulk->len) : 0);
    }

    return ret;
}

static abi_long apx_qemu_usbfs_proxy_do_ioctl(const IOCTLEntry *ie,
                                              uint8_t *buf_temp,
                                              int fd,
                                              int cmd,
                                              abi_long arg)
{
    switch (cmd) {
    case TARGET_USBDEVFS_CONTROL:
        return apx_usbfs_proxy_ioctl_control(ie, buf_temp, fd, arg);
    case TARGET_USBDEVFS_BULK:
        return apx_usbfs_proxy_ioctl_bulk(ie, buf_temp, fd, arg);
    case TARGET_USBDEVFS_CLAIMINTERFACE:
        return apx_usbfs_proxy_ioctl_int_arg(ie, buf_temp, fd, arg,
                                             APXR_OP_CLAIM_INTERFACE);
    case TARGET_USBDEVFS_RELEASEINTERFACE:
        return apx_usbfs_proxy_ioctl_int_arg(ie, buf_temp, fd, arg,
                                             APXR_OP_RELEASE_INTERFACE);
    case TARGET_USBDEVFS_RESET:
        return apx_usbfs_proxy_ioctl_reset(fd);
    default:
        return -TARGET_ENOTTY;
    }
}

#endif /* APX_QEMU_USBFS_PROXY_C */
