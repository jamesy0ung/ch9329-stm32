#include "ch9329_proto.h"

void ch9329_parser_init(struct ch9329_parser* p, ch9329_cmd_handler_t handler,
                        ch9329_reply_fn_t reply, void* user_data) {
    p->handler = handler;
    p->reply = reply;
    p->user_data = user_data;
    ch9329_parser_reset(p);
}

void ch9329_parser_reset(struct ch9329_parser* p) {
    p->state = CH9329_ST_HEAD0;
    p->cmd = 0;
    p->len = 0;
    p->idx = 0;
    p->sum = 0;
}

bool ch9329_parser_idle(const struct ch9329_parser* p) {
    return p->state == CH9329_ST_HEAD0;
}

size_t ch9329_build_reply(uint8_t cmd, uint8_t status,
                          uint8_t out[CH9329_REPLY_LEN]) {
    uint8_t sum = 0;

    out[0] = CH9329_HEAD0;
    out[1] = CH9329_HEAD1;
    out[2] = CH9329_ADDR;
    out[3] = cmd | (status == CH9329_STATUS_SUCCESS ? CH9329_REPLY_OK
                                                    : CH9329_REPLY_ERR);
    out[4] = 1;
    out[5] = status;

    for (int i = 0; i < CH9329_REPLY_LEN - 1; i++) {
        sum += out[i];
    }
    out[6] = sum;

    return CH9329_REPLY_LEN;
}

static void send_status(struct ch9329_parser* p, uint8_t status) {
    uint8_t frame[CH9329_REPLY_LEN];
    size_t len = ch9329_build_reply(p->cmd, status, frame);

    p->reply(frame, len, p->user_data);
}

/* Restart header scan; a stray 0x57 may itself begin the next frame. */
static void resync(struct ch9329_parser* p, uint8_t byte) {
    ch9329_parser_reset(p);
    if (byte == CH9329_HEAD0) {
        p->state = CH9329_ST_HEAD1;
        p->sum = byte;
    }
}

void ch9329_parser_feed(struct ch9329_parser* p, uint8_t byte) {
    switch (p->state) {
        case CH9329_ST_HEAD0:
            resync(p, byte);
            return;

        case CH9329_ST_HEAD1:
            if (byte != CH9329_HEAD1) {
                resync(p, byte);
                return;
            }
            p->state = CH9329_ST_ADDR;
            break;

        case CH9329_ST_ADDR:
            if (byte != CH9329_ADDR && byte != CH9329_ADDR_BCAST) {
                resync(p, byte);
                return;
            }
            p->state = CH9329_ST_CMD;
            break;

        case CH9329_ST_CMD:
            p->cmd = byte;
            p->state = CH9329_ST_LEN;
            break;

        case CH9329_ST_LEN:
            if (byte > CH9329_MAX_DATA) {
                /* Not a plausible frame; treat as line noise. */
                resync(p, byte);
                return;
            }
            p->len = byte;
            p->idx = 0;
            p->state = (byte == 0) ? CH9329_ST_SUM : CH9329_ST_DATA;
            break;

        case CH9329_ST_DATA:
            p->data[p->idx++] = byte;
            if (p->idx == p->len) {
                p->state = CH9329_ST_SUM;
            }
            break;

        case CH9329_ST_SUM: {
            uint8_t status;

            if (byte != p->sum) {
                status = CH9329_STATUS_ERR_SUM;
            } else {
                status = p->handler(p->cmd, p->data, p->len, p->user_data);
            }

            send_status(p, status);
            ch9329_parser_reset(p);
            return;
        }
    }

    p->sum += byte;
}
