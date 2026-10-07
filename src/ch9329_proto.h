/*
 * WCH CH9329 serial protocol framing.
 *
 * Frame: 57 AB | addr | cmd | len | data[len] | sum
 * where sum = (sum of every preceding byte) & 0xFF.
 *
 * This file has no Zephyr dependencies so it can be unit-tested on a host.
 */

#ifndef CH9329_PROTO_H_
#define CH9329_PROTO_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CH9329_HEAD0 0x57
#define CH9329_HEAD1 0xAB
#define CH9329_ADDR 0x00
#define CH9329_ADDR_BCAST 0xFF
#define CH9329_MAX_DATA 64
#define CH9329_REPLY_LEN 7

/* Commands */
#define CH9329_CMD_SEND_KB_GENERAL 0x02
#define CH9329_CMD_SEND_MS_ABS 0x04
#define CH9329_CMD_SEND_MS_REL 0x05
#define CH9329_CMD_RESET 0x0F

/* Reply cmd byte = cmd | REPLY_OK on success, cmd | REPLY_ERR on error */
#define CH9329_REPLY_OK 0x80
#define CH9329_REPLY_ERR 0xC0

/* Status codes carried in the 1-byte reply payload */
#define CH9329_STATUS_SUCCESS 0x00
#define CH9329_STATUS_ERR_TIMEOUT 0xE1
#define CH9329_STATUS_ERR_HEAD 0xE2
#define CH9329_STATUS_ERR_CMD 0xE3
#define CH9329_STATUS_ERR_SUM 0xE4
#define CH9329_STATUS_ERR_PARA 0xE5

/*
 * Called for every frame with a valid checksum. Returns the status code to
 * reply with. Must not block: replies are expected well within the host's
 * 300 ms timeout.
 */
typedef uint8_t (*ch9329_cmd_handler_t)(uint8_t cmd, const uint8_t* data,
                                        uint8_t len, void* user_data);

/* Called with a complete reply frame to transmit. */
typedef void (*ch9329_reply_fn_t)(const uint8_t* frame, size_t len,
                                  void* user_data);

enum ch9329_state {
    CH9329_ST_HEAD0,
    CH9329_ST_HEAD1,
    CH9329_ST_ADDR,
    CH9329_ST_CMD,
    CH9329_ST_LEN,
    CH9329_ST_DATA,
    CH9329_ST_SUM,
};

struct ch9329_parser {
    enum ch9329_state state;
    uint8_t cmd;
    uint8_t len;
    uint8_t idx;
    uint8_t sum;
    uint8_t data[CH9329_MAX_DATA];

    ch9329_cmd_handler_t handler;
    ch9329_reply_fn_t reply;
    void* user_data;
};

void ch9329_parser_init(struct ch9329_parser* p, ch9329_cmd_handler_t handler,
                        ch9329_reply_fn_t reply, void* user_data);

/* Discard any partial frame and start scanning for 57 AB again. */
void ch9329_parser_reset(struct ch9329_parser* p);

/* True when no partial frame is buffered. */
bool ch9329_parser_idle(const struct ch9329_parser* p);

void ch9329_parser_feed(struct ch9329_parser* p, uint8_t byte);

/* Build a 7-byte status reply for cmd. Returns the frame length. */
size_t ch9329_build_reply(uint8_t cmd, uint8_t status,
                          uint8_t out[CH9329_REPLY_LEN]);

#endif /* CH9329_PROTO_H_ */
