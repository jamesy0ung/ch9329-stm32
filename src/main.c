/*
 * CH9329 emulator: CH9329 serial frames on USART1 (ST-Link VCP) are turned
 * into USB HID reports on OTG FS, so DezKVM-Go can drive this board as if it
 * were a CH340 + CH9329.
 */

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/ring_buffer.h>

#include "ch9329_proto.h"
#include "hid_out.h"
#include "usb_dev.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

static const struct device* const uart_dev =
    DEVICE_DT_GET(DT_CHOSEN(dezkvm_ch9329_uart));

RING_BUF_DECLARE(rx_ring, 256);
static K_SEM_DEFINE(rx_sem, 0, 1);
static struct k_spinlock rx_lock;

static struct ch9329_parser parser;

static void uart_isr(const struct device* dev, void* user_data) {
    uint8_t buf[16];
    int n;

    ARG_UNUSED(user_data);

    uart_irq_update(dev);

    while (uart_irq_rx_ready(dev)) {
        n = uart_fifo_read(dev, buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
        /* On overflow the excess is dropped; the parser resyncs. */
        (void)ring_buf_put(&rx_ring, buf, n);
        k_sem_give(&rx_sem);
    }
}

static void reply_tx(const uint8_t* frame, size_t len, void* user_data) {
    ARG_UNUSED(user_data);

    /* 7 bytes at 115200 baud is ~0.6 ms; polling keeps this simple. */
    for (size_t i = 0; i < len; i++) {
        uart_poll_out(uart_dev, frame[i]);
    }
}

static uint8_t handle_cmd(uint8_t cmd, const uint8_t* data, uint8_t len,
                          void* user_data) {
    ARG_UNUSED(user_data);

    switch (cmd) {
        case CH9329_CMD_SEND_KB_GENERAL:
            /* [mod, 0x00, k1..k6] is already a boot keyboard report. */
            if (len != HID_KB_REPORT_LEN) {
                return CH9329_STATUS_ERR_PARA;
            }
            hid_out_keyboard(data);
            return CH9329_STATUS_SUCCESS;

        case CH9329_CMD_SEND_MS_ABS:
            /* [0x02, buttons, xL, xH, yL, yH, wheel] */
            if (len != 7 || data[0] != 0x02) {
                return CH9329_STATUS_ERR_PARA;
            }
            hid_out_mouse_abs(data[1], sys_get_le16(&data[2]),
                              sys_get_le16(&data[4]), (int8_t)data[6]);
            return CH9329_STATUS_SUCCESS;

        case CH9329_CMD_SEND_MS_REL:
            /* [0x01, buttons, dx, dy, wheel] */
            if (len != 5 || data[0] != 0x01) {
                return CH9329_STATUS_ERR_PARA;
            }
            hid_out_mouse_rel(data[1], (int8_t)data[2], (int8_t)data[3],
                              (int8_t)data[4]);
            return CH9329_STATUS_SUCCESS;

        case CH9329_CMD_RESET:
            hid_out_release_all();
            return CH9329_STATUS_SUCCESS;

        default:
            LOG_WRN("unsupported cmd 0x%02x len %u", cmd, len);
            return CH9329_STATUS_ERR_CMD;
    }
}

int main(void) {
    uint8_t buf[32];
    uint32_t n;
    int ret;

    if (!device_is_ready(uart_dev)) {
        LOG_ERR("CH9329 UART not ready");
        return -ENODEV;
    }

    ret = hid_out_init();
    if (ret != 0) {
        LOG_ERR("HID init failed, %d", ret);
        return ret;
    }

    ret = usb_dev_init();
    if (ret != 0) {
        LOG_ERR("USB init failed, %d", ret);
        return ret;
    }

    ch9329_parser_init(&parser, handle_cmd, reply_tx, NULL);

    ret = uart_irq_callback_user_data_set(uart_dev, uart_isr, NULL);
    if (ret != 0) {
        LOG_ERR("UART IRQ setup failed, %d", ret);
        return ret;
    }
    uart_irq_rx_enable(uart_dev);

    LOG_INF("CH9329 emulator ready");

    while (true) {
        k_timeout_t timeout = ch9329_parser_idle(&parser)
                                  ? K_FOREVER
                                  : K_MSEC(CONFIG_APP_FRAME_TIMEOUT_MS);

        if (k_sem_take(&rx_sem, timeout) != 0) {
            /* Inter-byte silence mid-frame: drop the partial frame. */
            LOG_DBG("frame timeout, resync");
            ch9329_parser_reset(&parser);
            continue;
        }

        do {
            K_SPINLOCK(&rx_lock) {
                n = ring_buf_get(&rx_ring, buf, sizeof(buf));
            }
            for (uint32_t i = 0; i < n; i++) {
                ch9329_parser_feed(&parser, buf[i]);
            }
        } while (n > 0);
    }

    return 0;
}
