#ifndef APX_RELAY_PROTOCOL_H
#define APX_RELAY_PROTOCOL_H

#include <stdint.h>

/*
 * APX relay protocol v1
 *
 * Transport:
 *   TCP, default bind 127.0.0.1:17523.
 *   Each frame is length-prefixed:
 *     u32be frame_len
 *     frame_len bytes of message
 *
 * Message header, included in frame_len:
 *   u32be magic        ASCII "APXR" (0x41505852)
 *   u16be version      APXR_PROTOCOL_VERSION
 *   u16be opcode       enum apxr_opcode
 *   u32be seq          client-selected sequence number, echoed in response
 *   i32be status       request: 0; response: >=0 success, <0 error
 *   u32be payload_len  bytes after this 20-byte header
 *   u8[payload_len] payload
 *
 * Multi-byte fields are network byte order.  Status uses signed two's
 * complement in the i32be slot.  libusb errors are returned directly when
 * possible; daemon-local errors use APXR_STATUS_* values below.
 *
 * Command payloads:
 *   PING: arbitrary bytes, echoed.
 *
 *   OPEN request, 12 bytes:
 *     u16be vid        0 means APXR_NVIDIA_VID
 *     u16be pid        0 means APXR_APX_PID
 *     u32be ordinal    nth matching device after filters, usually 0
 *     u8    bus        0 means wildcard
 *     u8    address    0 means wildcard
 *     u16be reserved   0
 *   OPEN response payload, 8 bytes on success:
 *     u16be vid, u16be pid, u8 bus, u8 address, u16be reserved
 *
 *   CLAIM_INTERFACE / RELEASE_INTERFACE request, 4 bytes:
 *     u8 interface_number, u8[3] reserved
 *
 *   CONTROL_TRANSFER request, 12 + optional OUT data bytes:
 *     u8 bmRequestType, u8 bRequest, u16be wValue, u16be wIndex,
 *     u16be wLength, u32be timeout_ms, then wLength bytes for OUT transfers.
 *     IN transfer responses carry the received bytes as payload.
 *
 *   BULK_READ request, 12 bytes:
 *     u8 endpoint, u8 reserved, u16be reserved, u32be length,
 *     u32be timeout_ms.  Response payload is received bytes.
 *
 *   BULK_WRITE request, 8 + data bytes:
 *     u8 endpoint, u8 reserved, u16be reserved, u32be timeout_ms, data bytes.
 *     Response status is the libusb transferred byte count on success.
 *
 *   RESET_DEVICE / CLOSE request: empty payload.
 *
 * The daemon never detaches kernel drivers.  Interface ownership is changed
 * only by explicit CLAIM_INTERFACE / RELEASE_INTERFACE requests, and all
 * claimed interfaces are released before closing a handle.
 */

#define APXR_MAGIC 0x41505852u
#define APXR_PROTOCOL_VERSION 1u
#define APXR_HEADER_SIZE 20u
#define APXR_MAX_FRAME_SIZE (16u * 1024u * 1024u)
#define APXR_MAX_INTERFACES 256u

#define APXR_DEFAULT_BIND "127.0.0.1"
#define APXR_DEFAULT_PORT 17523

#define APXR_NVIDIA_VID 0x0955u
#define APXR_APX_PID 0x7523u

enum apxr_opcode {
    APXR_OP_PING = 1,
    APXR_OP_OPEN = 2,
    APXR_OP_CLAIM_INTERFACE = 3,
    APXR_OP_RELEASE_INTERFACE = 4,
    APXR_OP_CONTROL_TRANSFER = 5,
    APXR_OP_BULK_READ = 6,
    APXR_OP_BULK_WRITE = 7,
    APXR_OP_RESET_DEVICE = 8,
    APXR_OP_CLOSE = 9
};

enum apxr_status {
    APXR_STATUS_OK = 0,
    APXR_STATUS_BAD_FRAME = -1000,
    APXR_STATUS_BAD_VERSION = -1001,
    APXR_STATUS_BAD_OPCODE = -1002,
    APXR_STATUS_BAD_LENGTH = -1003,
    APXR_STATUS_NOT_OPEN = -1004,
    APXR_STATUS_ALREADY_OPEN = -1005,
    APXR_STATUS_NOT_FOUND = -1006,
    APXR_STATUS_NO_MEMORY = -1007,
    APXR_STATUS_IO = -1008,
    APXR_STATUS_BAD_ARGUMENT = -1009
};

#endif /* APX_RELAY_PROTOCOL_H */
