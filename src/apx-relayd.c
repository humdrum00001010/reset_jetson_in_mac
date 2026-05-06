#include "apx_relay_protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <libusb.h>

struct relay_state {
    libusb_context *usb;
    libusb_device_handle *handle;
    unsigned char claimed[APXR_MAX_INTERFACES];
};

static uint16_t read_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint32_t read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

static void write_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void write_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static int read_exact(int fd, void *buf, size_t len)
{
    uint8_t *p = (uint8_t *)buf;

    while (len > 0) {
        ssize_t n = recv(fd, p, len, 0);
        if (n == 0) {
            return 0;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        p += (size_t)n;
        len -= (size_t)n;
    }

    return 1;
}

static int write_exact(int fd, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;

    while (len > 0) {
        ssize_t n = send(fd, p, len, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        p += (size_t)n;
        len -= (size_t)n;
    }

    return 0;
}

static int send_response(int fd, uint16_t opcode, uint32_t seq, int32_t status,
                         const uint8_t *payload, uint32_t payload_len)
{
    uint8_t prefix[4];
    uint8_t header[APXR_HEADER_SIZE];
    uint32_t frame_len = APXR_HEADER_SIZE + payload_len;

    if (frame_len > APXR_MAX_FRAME_SIZE) {
        return -1;
    }

    write_be32(prefix, frame_len);
    write_be32(header + 0, APXR_MAGIC);
    write_be16(header + 4, APXR_PROTOCOL_VERSION);
    write_be16(header + 6, opcode);
    write_be32(header + 8, seq);
    write_be32(header + 12, (uint32_t)status);
    write_be32(header + 16, payload_len);

    if (write_exact(fd, prefix, sizeof(prefix)) < 0) {
        return -1;
    }
    if (write_exact(fd, header, sizeof(header)) < 0) {
        return -1;
    }
    if (payload_len > 0 && write_exact(fd, payload, payload_len) < 0) {
        return -1;
    }

    return 0;
}

static void close_device(struct relay_state *state)
{
    if (state->handle == NULL) {
        return;
    }

    for (size_t i = 0; i < APXR_MAX_INTERFACES; i++) {
        if (state->claimed[i]) {
            (void)libusb_release_interface(state->handle, (int)i);
            state->claimed[i] = 0;
        }
    }

    libusb_close(state->handle);
    state->handle = NULL;
}

static int alloc_response(uint8_t **out, uint32_t len)
{
    if (len == 0) {
        *out = NULL;
        return APXR_STATUS_OK;
    }

    *out = (uint8_t *)malloc(len);
    if (*out == NULL) {
        return APXR_STATUS_NO_MEMORY;
    }

    return APXR_STATUS_OK;
}

static int handle_ping(const uint8_t *payload, uint32_t payload_len,
                       uint8_t **response, uint32_t *response_len)
{
    int r = alloc_response(response, payload_len);
    if (r < 0) {
        return r;
    }
    if (payload_len > 0) {
        memcpy(*response, payload, payload_len);
    }
    *response_len = payload_len;
    return APXR_STATUS_OK;
}

static int handle_open(struct relay_state *state, const uint8_t *payload,
                       uint32_t payload_len, uint8_t **response,
                       uint32_t *response_len)
{
    uint16_t vid;
    uint16_t pid;
    uint32_t ordinal;
    uint8_t bus_filter;
    uint8_t address_filter;
    uint32_t seen = 0;
    libusb_device **devices = NULL;
    ssize_t count;
    int status = APXR_STATUS_NOT_FOUND;

    if (payload_len != 12) {
        return APXR_STATUS_BAD_LENGTH;
    }
    if (state->handle != NULL) {
        return APXR_STATUS_ALREADY_OPEN;
    }

    vid = read_be16(payload + 0);
    pid = read_be16(payload + 2);
    ordinal = read_be32(payload + 4);
    bus_filter = payload[8];
    address_filter = payload[9];

    if (vid == 0) {
        vid = APXR_NVIDIA_VID;
    }
    if (pid == 0) {
        pid = APXR_APX_PID;
    }

    fprintf(stderr, "APXR OPEN start\n");
    fflush(stderr);

    count = libusb_get_device_list(state->usb, &devices);
    if (count < 0) {
        fprintf(stderr, "APXR OPEN libusb_get_device_list status=%zd\n", count);
        fflush(stderr);
        return (int)count;
    }

    for (ssize_t i = 0; i < count; i++) {
        struct libusb_device_descriptor desc;
        libusb_device *dev = devices[i];
        uint8_t bus;
        uint8_t address;
        int r;

        r = libusb_get_device_descriptor(dev, &desc);
        if (r < 0) {
            continue;
        }
        if (desc.idVendor != vid || desc.idProduct != pid) {
            continue;
        }

        bus = libusb_get_bus_number(dev);
        address = libusb_get_device_address(dev);
        if (bus_filter != 0 && bus_filter != bus) {
            continue;
        }
        if (address_filter != 0 && address_filter != address) {
            continue;
        }
        if (seen != ordinal) {
            seen++;
            continue;
        }

        fprintf(stderr, "APXR OPEN match vid=%04x pid=%04x bus=%u address=%u\n",
                vid, pid, bus, address);
        fflush(stderr);

        r = libusb_open(dev, &state->handle);
        fprintf(stderr, "APXR OPEN libusb_open status=%d\n", r);
        fflush(stderr);
        if (r < 0) {
            status = r;
            break;
        }

        status = alloc_response(response, 8);
        if (status < 0) {
            close_device(state);
            break;
        }

        write_be16(*response + 0, vid);
        write_be16(*response + 2, pid);
        (*response)[4] = bus;
        (*response)[5] = address;
        write_be16(*response + 6, 0);
        *response_len = 8;
        status = APXR_STATUS_OK;
        break;
    }

    libusb_free_device_list(devices, 1);
    return status;
}

static int handle_claim(struct relay_state *state, const uint8_t *payload,
                        uint32_t payload_len)
{
    uint8_t iface;
    int r;

    if (payload_len != 4) {
        return APXR_STATUS_BAD_LENGTH;
    }
    if (state->handle == NULL) {
        return APXR_STATUS_NOT_OPEN;
    }

    iface = payload[0];
    fprintf(stderr, "APXR CLAIM iface=%u\n", iface);
    fflush(stderr);
    r = libusb_claim_interface(state->handle, (int)iface);
    fprintf(stderr, "APXR CLAIM status=%d\n", r);
    fflush(stderr);
    if (r == 0) {
        state->claimed[iface] = 1;
    }

    return r;
}

static int handle_release(struct relay_state *state, const uint8_t *payload,
                          uint32_t payload_len)
{
    uint8_t iface;
    int r;

    if (payload_len != 4) {
        return APXR_STATUS_BAD_LENGTH;
    }
    if (state->handle == NULL) {
        return APXR_STATUS_NOT_OPEN;
    }

    iface = payload[0];
    r = libusb_release_interface(state->handle, (int)iface);
    if (r == 0) {
        state->claimed[iface] = 0;
    }

    return r;
}

static int handle_control(struct relay_state *state, const uint8_t *payload,
                          uint32_t payload_len, uint8_t **response,
                          uint32_t *response_len)
{
    uint8_t bm_request_type;
    uint8_t request;
    uint16_t value;
    uint16_t index;
    uint16_t length;
    uint32_t timeout_ms;
    bool direction_in;
    uint8_t *buffer = NULL;
    int r;

    if (payload_len < 12) {
        return APXR_STATUS_BAD_LENGTH;
    }
    if (state->handle == NULL) {
        return APXR_STATUS_NOT_OPEN;
    }

    bm_request_type = payload[0];
    request = payload[1];
    value = read_be16(payload + 2);
    index = read_be16(payload + 4);
    length = read_be16(payload + 6);
    timeout_ms = read_be32(payload + 8);
    direction_in = (bm_request_type & LIBUSB_ENDPOINT_IN) != 0;

    if (direction_in) {
        if (payload_len != 12) {
            return APXR_STATUS_BAD_LENGTH;
        }
    } else if (payload_len != (uint32_t)12 + (uint32_t)length) {
        return APXR_STATUS_BAD_LENGTH;
    }

    if (length > 0) {
        buffer = (uint8_t *)malloc(length);
        if (buffer == NULL) {
            return APXR_STATUS_NO_MEMORY;
        }
        if (!direction_in) {
            memcpy(buffer, payload + 12, length);
        }
    }

    fprintf(stderr,
            "APXR CONTROL type=0x%02x req=0x%02x value=0x%04x index=0x%04x len=%u timeout=%u\n",
            bm_request_type, request, value, index, length, timeout_ms);
    fflush(stderr);
    r = libusb_control_transfer(state->handle, bm_request_type, request,
                                value, index, buffer, length, timeout_ms);
    fprintf(stderr, "APXR CONTROL status=%d\n", r);
    fflush(stderr);
    if (r >= 0 && direction_in && r > 0) {
        int status = alloc_response(response, (uint32_t)r);
        if (status < 0) {
            free(buffer);
            return status;
        }
        memcpy(*response, buffer, (size_t)r);
        *response_len = (uint32_t)r;
    }

    free(buffer);
    return r;
}

static int handle_bulk_read(struct relay_state *state, const uint8_t *payload,
                            uint32_t payload_len, uint8_t **response,
                            uint32_t *response_len)
{
    uint8_t endpoint;
    uint32_t length;
    uint32_t timeout_ms;
    uint8_t *buffer = NULL;
    int transferred = 0;
    int r;

    if (payload_len != 12) {
        return APXR_STATUS_BAD_LENGTH;
    }
    if (state->handle == NULL) {
        return APXR_STATUS_NOT_OPEN;
    }

    endpoint = payload[0];
    length = read_be32(payload + 4);
    timeout_ms = read_be32(payload + 8);
    if (length > APXR_MAX_FRAME_SIZE - APXR_HEADER_SIZE || length > INT_MAX) {
        return APXR_STATUS_BAD_ARGUMENT;
    }
    if (length == 0) {
        *response = NULL;
        *response_len = 0;
        return 0;
    }

    buffer = (uint8_t *)malloc(length);
    if (buffer == NULL) {
        return APXR_STATUS_NO_MEMORY;
    }

    fprintf(stderr, "APXR BULK_READ ep=0x%02x len=%u timeout=%u\n",
            endpoint, length, timeout_ms);
    fflush(stderr);
    r = libusb_bulk_transfer(state->handle, endpoint, buffer, (int)length,
                             &transferred, timeout_ms);
    fprintf(stderr, "APXR BULK_READ status=%d transferred=%d\n", r, transferred);
    fflush(stderr);
    if (r == 0 && transferred > 0) {
        int status = alloc_response(response, (uint32_t)transferred);
        if (status < 0) {
            free(buffer);
            return status;
        }
        memcpy(*response, buffer, (size_t)transferred);
        *response_len = (uint32_t)transferred;
        r = transferred;
    }

    free(buffer);
    return r;
}

static int handle_bulk_write(struct relay_state *state, const uint8_t *payload,
                             uint32_t payload_len)
{
    uint8_t endpoint;
    uint32_t timeout_ms;
    uint32_t length;
    int transferred = 0;
    int r;

    if (payload_len < 8) {
        return APXR_STATUS_BAD_LENGTH;
    }
    if (state->handle == NULL) {
        return APXR_STATUS_NOT_OPEN;
    }

    endpoint = payload[0];
    timeout_ms = read_be32(payload + 4);
    length = payload_len - 8;
    if (length > INT_MAX) {
        return APXR_STATUS_BAD_ARGUMENT;
    }
    if (length == 0) {
        return 0;
    }

    fprintf(stderr, "APXR BULK_WRITE ep=0x%02x len=%u timeout=%u\n",
            endpoint, length, timeout_ms);
    fflush(stderr);
    r = libusb_bulk_transfer(state->handle, endpoint, (uint8_t *)(payload + 8),
                             (int)length, &transferred, timeout_ms);
    fprintf(stderr, "APXR BULK_WRITE status=%d transferred=%d\n", r, transferred);
    fflush(stderr);
    if (r == 0) {
        return transferred;
    }

    return r;
}

static int dispatch_message(struct relay_state *state, uint16_t opcode,
                            const uint8_t *payload, uint32_t payload_len,
                            uint8_t **response, uint32_t *response_len)
{
    *response = NULL;
    *response_len = 0;

    switch (opcode) {
    case APXR_OP_PING:
        return handle_ping(payload, payload_len, response, response_len);
    case APXR_OP_OPEN:
        return handle_open(state, payload, payload_len, response, response_len);
    case APXR_OP_CLAIM_INTERFACE:
        return handle_claim(state, payload, payload_len);
    case APXR_OP_RELEASE_INTERFACE:
        return handle_release(state, payload, payload_len);
    case APXR_OP_CONTROL_TRANSFER:
        return handle_control(state, payload, payload_len, response, response_len);
    case APXR_OP_BULK_READ:
        return handle_bulk_read(state, payload, payload_len, response, response_len);
    case APXR_OP_BULK_WRITE:
        return handle_bulk_write(state, payload, payload_len);
    case APXR_OP_RESET_DEVICE:
        if (payload_len != 0) {
            return APXR_STATUS_BAD_LENGTH;
        }
        if (state->handle == NULL) {
            return APXR_STATUS_NOT_OPEN;
        }
        return libusb_reset_device(state->handle);
    case APXR_OP_CLOSE:
        if (payload_len != 0) {
            return APXR_STATUS_BAD_LENGTH;
        }
        close_device(state);
        return APXR_STATUS_OK;
    default:
        return APXR_STATUS_BAD_OPCODE;
    }
}

static int process_frame(int client_fd, struct relay_state *state,
                         const uint8_t *frame, uint32_t frame_len)
{
    uint32_t magic;
    uint16_t version;
    uint16_t opcode;
    uint32_t seq;
    uint32_t payload_len;
    const uint8_t *payload;
    uint8_t *response = NULL;
    uint32_t response_len = 0;
    int status;
    int send_status;

    if (frame_len < APXR_HEADER_SIZE) {
        return send_response(client_fd, 0, 0, APXR_STATUS_BAD_FRAME, NULL, 0);
    }

    magic = read_be32(frame + 0);
    version = read_be16(frame + 4);
    opcode = read_be16(frame + 6);
    seq = read_be32(frame + 8);
    payload_len = read_be32(frame + 16);

    if (magic != APXR_MAGIC) {
        return send_response(client_fd, opcode, seq, APXR_STATUS_BAD_FRAME,
                             NULL, 0);
    }
    if (version != APXR_PROTOCOL_VERSION) {
        return send_response(client_fd, opcode, seq, APXR_STATUS_BAD_VERSION,
                             NULL, 0);
    }
    if (payload_len != frame_len - APXR_HEADER_SIZE) {
        return send_response(client_fd, opcode, seq, APXR_STATUS_BAD_LENGTH,
                             NULL, 0);
    }

    payload = frame + APXR_HEADER_SIZE;
    fprintf(stderr, "APXR REQUEST opcode=%u seq=%u payload_len=%u\n",
            opcode, seq, payload_len);
    fflush(stderr);
    status = dispatch_message(state, opcode, payload, payload_len,
                              &response, &response_len);
    fprintf(stderr, "APXR RESPONSE opcode=%u seq=%u status=%d response_len=%u\n",
            opcode, seq, status, response_len);
    fflush(stderr);
    if (status < 0) {
        free(response);
        response = NULL;
        response_len = 0;
    }

    send_status = send_response(client_fd, opcode, seq, status, response,
                                response_len);
    free(response);
    return send_status;
}

static void serve_client(int client_fd, libusb_context *usb)
{
    struct relay_state state;

    memset(&state, 0, sizeof(state));
    state.usb = usb;

    for (;;) {
        uint8_t prefix[4];
        uint32_t frame_len;
        uint8_t *frame;
        int r;

        r = read_exact(client_fd, prefix, sizeof(prefix));
        if (r <= 0) {
            break;
        }

        frame_len = read_be32(prefix);
        if (frame_len < APXR_HEADER_SIZE || frame_len > APXR_MAX_FRAME_SIZE) {
            break;
        }

        frame = (uint8_t *)malloc(frame_len);
        if (frame == NULL) {
            (void)send_response(client_fd, 0, 0, APXR_STATUS_NO_MEMORY, NULL, 0);
            break;
        }

        r = read_exact(client_fd, frame, frame_len);
        if (r <= 0) {
            free(frame);
            break;
        }

        r = process_frame(client_fd, &state, frame, frame_len);
        free(frame);
        if (r < 0) {
            break;
        }
    }

    close_device(&state);
    close(client_fd);
}

static int parse_port(const char *s, uint16_t *port)
{
    char *end = NULL;
    long value;

    errno = 0;
    value = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || value < 1 || value > 65535) {
        return -1;
    }

    *port = (uint16_t)value;
    return 0;
}

static void usage(const char *argv0)
{
    fprintf(stderr, "usage: %s [-b bind-address] [-p port]\n", argv0);
    fprintf(stderr, "default: %s:%u\n", APXR_DEFAULT_BIND, APXR_DEFAULT_PORT);
}

int main(int argc, char **argv)
{
    const char *bind_address = APXR_DEFAULT_BIND;
    uint16_t port = APXR_DEFAULT_PORT;
    libusb_context *usb = NULL;
    int server_fd = -1;
    int opt = 1;
    int ch;
    int r;
    struct sockaddr_in addr;

    while ((ch = getopt(argc, argv, "b:p:h")) != -1) {
        switch (ch) {
        case 'b':
            bind_address = optarg;
            break;
        case 'p':
            if (parse_port(optarg, &port) < 0) {
                usage(argv[0]);
                return 2;
            }
            break;
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            usage(argv[0]);
            return 2;
        }
    }

    signal(SIGPIPE, SIG_IGN);

    r = libusb_init(&usb);
    if (r < 0) {
        fprintf(stderr, "libusb_init: %s\n", libusb_error_name(r));
        return 1;
    }

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        libusb_exit(usb);
        return 1;
    }

    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt(SO_REUSEADDR)");
        close(server_fd);
        libusb_exit(usb);
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, bind_address, &addr.sin_addr) != 1) {
        fprintf(stderr, "invalid IPv4 bind address: %s\n", bind_address);
        close(server_fd);
        libusb_exit(usb);
        return 2;
    }

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(server_fd);
        libusb_exit(usb);
        return 1;
    }

    if (listen(server_fd, 4) < 0) {
        perror("listen");
        close(server_fd);
        libusb_exit(usb);
        return 1;
    }

    fprintf(stderr, "apx-relayd listening on %s:%u\n", bind_address, port);

    for (;;) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            break;
        }
        serve_client(client_fd, usb);
    }

    close(server_fd);
    libusb_exit(usb);
    return 1;
}
