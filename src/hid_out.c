/*
 * USB HID report output.
 *
 * Each HID interface is a channel with a small FIFO of pending reports and
 * one transfer buffer. Callers never wait for USB: a report is queued and the
 * next one is submitted from the input_report_done() callback. Providing that
 * callback is what makes hid_device_submit_report() asynchronous; without it
 * the call blocks until the host polls, which could stall the UART replies.
 *
 * When the FIFO is full the oldest report is dropped, so the most recent
 * state (e.g. "all keys released") always reaches the host.
 */

#include "hid_out.h"

#include <string.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/usb/class/usbd_hid.h>
#include <zephyr/usb/usbd.h>

#include "usb_dev.h"

LOG_MODULE_REGISTER(hid_out, LOG_LEVEL_INF);

#define HID_MAX_REPORT_LEN 8
#define HID_QUEUE_LEN 8

struct hid_chan {
    const struct device* dev;
    const uint8_t* rdesc;
    uint16_t rdesc_len;
    uint8_t report_len;
    uint8_t* tx;

    struct k_spinlock lock;
    bool ready;
    bool busy;
    uint8_t head;
    uint8_t count;
    uint8_t queue[HID_QUEUE_LEN][HID_MAX_REPORT_LEN];
    /* Last report handed to USB, returned for Get_Report(Input). */
    uint8_t last[HID_MAX_REPORT_LEN];
};

/* Keyboard */

static const uint8_t kbd_rdesc[] = HID_KEYBOARD_REPORT_DESC();
UDC_STATIC_BUF_DEFINE(kbd_tx, HID_KB_REPORT_LEN);

/* Relative mouse: [buttons, dx, dy, wheel], boot-mouse compatible */

#define MOUSE_REL_REPORT_LEN 4

static const uint8_t mouse_rel_rdesc[] = HID_MOUSE_REPORT_DESC(3);
UDC_STATIC_BUF_DEFINE(mouse_rel_tx, MOUSE_REL_REPORT_LEN);

/* Absolute mouse: [buttons, xL, xH, yL, yH, wheel], X/Y 0..4095 */

#define MOUSE_ABS_REPORT_LEN 6
#define MOUSE_ABS_MAX 4095

static const uint8_t mouse_abs_rdesc[] = {
    HID_USAGE_PAGE(HID_USAGE_GEN_DESKTOP),
    HID_USAGE(HID_USAGE_GEN_DESKTOP_MOUSE),
    HID_COLLECTION(HID_COLLECTION_APPLICATION),
    HID_USAGE(HID_USAGE_GEN_DESKTOP_POINTER),
    HID_COLLECTION(HID_COLLECTION_PHYSICAL),
    /* 3 buttons + 5 bits padding */
    HID_USAGE_PAGE(HID_USAGE_GEN_BUTTON),
    HID_USAGE_MIN8(1),
    HID_USAGE_MAX8(3),
    HID_LOGICAL_MIN8(0),
    HID_LOGICAL_MAX8(1),
    HID_REPORT_SIZE(1),
    HID_REPORT_COUNT(3),
    /* HID_INPUT (Data,Var,Abs) */
    HID_INPUT(0x02),
    HID_REPORT_SIZE(5),
    HID_REPORT_COUNT(1),
    /* HID_INPUT (Cnst,Ary,Abs) */
    HID_INPUT(0x01),
    /* X, Y: uint16 0..4095 */
    HID_USAGE_PAGE(HID_USAGE_GEN_DESKTOP),
    HID_USAGE(HID_USAGE_GEN_DESKTOP_X),
    HID_USAGE(HID_USAGE_GEN_DESKTOP_Y),
    HID_LOGICAL_MIN8(0),
    HID_LOGICAL_MAX16(MOUSE_ABS_MAX & 0xFF, MOUSE_ABS_MAX >> 8),
    HID_REPORT_SIZE(16),
    HID_REPORT_COUNT(2),
    /* HID_INPUT (Data,Var,Abs) */
    HID_INPUT(0x02),
    /* Wheel: int8 */
    HID_USAGE(HID_USAGE_GEN_DESKTOP_WHEEL),
    HID_LOGICAL_MIN8(-127),
    HID_LOGICAL_MAX8(127),
    HID_REPORT_SIZE(8),
    HID_REPORT_COUNT(1),
    /* HID_INPUT (Data,Var,Rel) */
    HID_INPUT(0x06),
    HID_END_COLLECTION,
    HID_END_COLLECTION,
};
UDC_STATIC_BUF_DEFINE(mouse_abs_tx, MOUSE_ABS_REPORT_LEN);

enum {
    CHAN_KBD,
    CHAN_MOUSE_REL,
    CHAN_MOUSE_ABS,
    CHAN_COUNT,
};

static struct hid_chan chans[CHAN_COUNT] = {
    [CHAN_KBD] =
        {
            .dev = DEVICE_DT_GET(DT_NODELABEL(hid_kbd)),
            .rdesc = kbd_rdesc,
            .rdesc_len = sizeof(kbd_rdesc),
            .report_len = HID_KB_REPORT_LEN,
            .tx = kbd_tx,
        },
    [CHAN_MOUSE_REL] =
        {
            .dev = DEVICE_DT_GET(DT_NODELABEL(hid_mouse_rel)),
            .rdesc = mouse_rel_rdesc,
            .rdesc_len = sizeof(mouse_rel_rdesc),
            .report_len = MOUSE_REL_REPORT_LEN,
            .tx = mouse_rel_tx,
        },
    [CHAN_MOUSE_ABS] =
        {
            .dev = DEVICE_DT_GET(DT_NODELABEL(hid_mouse_abs)),
            .rdesc = mouse_abs_rdesc,
            .rdesc_len = sizeof(mouse_abs_rdesc),
            .report_len = MOUSE_ABS_REPORT_LEN,
            .tx = mouse_abs_tx,
        },
};

static struct hid_chan* chan_from_dev(const struct device* dev) {
    for (size_t i = 0; i < ARRAY_SIZE(chans); i++) {
        if (chans[i].dev == dev) {
            return &chans[i];
        }
    }

    return NULL;
}

/* Submit the next queued report if no transfer is in flight. */
static void chan_kick(struct hid_chan* ch) {
    k_spinlock_key_t key = k_spin_lock(&ch->lock);
    int ret;

    if (!ch->ready || ch->busy || ch->count == 0) {
        k_spin_unlock(&ch->lock, key);
        return;
    }

    memcpy(ch->tx, ch->queue[ch->head], ch->report_len);
    memcpy(ch->last, ch->tx, ch->report_len);
    ch->head = (ch->head + 1) % HID_QUEUE_LEN;
    ch->count--;
    ch->busy = true;
    k_spin_unlock(&ch->lock, key);

    ret = hid_device_submit_report(ch->dev, ch->report_len, ch->tx);
    if (ret != 0) {
        LOG_WRN("%s: submit failed (%d), flushing", ch->dev->name, ret);
        key = k_spin_lock(&ch->lock);
        ch->busy = false;
        ch->count = 0;
        k_spin_unlock(&ch->lock, key);
    }
}

static void chan_put(struct hid_chan* ch, const uint8_t* report) {
    k_spinlock_key_t key = k_spin_lock(&ch->lock);
    uint8_t tail;

    if (!ch->ready) {
        /* Not configured by the host: drop, the reply is still OK. */
        k_spin_unlock(&ch->lock, key);
        return;
    }

    if (ch->count == HID_QUEUE_LEN) {
        ch->head = (ch->head + 1) % HID_QUEUE_LEN;
        ch->count--;
    }

    tail = (ch->head + ch->count) % HID_QUEUE_LEN;
    memcpy(ch->queue[tail], report, ch->report_len);
    ch->count++;
    k_spin_unlock(&ch->lock, key);

    chan_kick(ch);
}

/* hid_device_ops (called from the USB stack thread, must not block) */

static void hid_iface_ready(const struct device* dev, const bool ready) {
    struct hid_chan* ch = chan_from_dev(dev);
    k_spinlock_key_t key;

    LOG_INF("%s %s", dev->name, ready ? "ready" : "not ready");

    key = k_spin_lock(&ch->lock);
    ch->ready = ready;
    ch->busy = false;
    ch->count = 0;
    k_spin_unlock(&ch->lock, key);
}

static void hid_input_report_done(const struct device* dev,
                                  const uint8_t* const report) {
    struct hid_chan* ch = chan_from_dev(dev);
    k_spinlock_key_t key = k_spin_lock(&ch->lock);

    ch->busy = false;
    k_spin_unlock(&ch->lock, key);

    chan_kick(ch);
}

static int hid_get_report(const struct device* dev, const uint8_t type,
                          const uint8_t id, const uint16_t len,
                          uint8_t* const buf) {
    struct hid_chan* ch = chan_from_dev(dev);
    uint16_t n = MIN(len, ch->report_len);

    if (type != HID_REPORT_TYPE_INPUT || id != 0) {
        return -ENOTSUP;
    }

    memcpy(buf, ch->last, n);

    return n;
}

static int hid_set_report(const struct device* dev, const uint8_t type,
                          const uint8_t id, const uint16_t len,
                          const uint8_t* const buf) {
    /* Keyboard LED output report: accepted and ignored. */
    if (type != HID_REPORT_TYPE_OUTPUT) {
        return -ENOTSUP;
    }

    return 0;
}

static void hid_set_idle(const struct device* dev, const uint8_t id,
                         const uint32_t duration) {
    /*
     * Must be present or the class stalls SET_IDLE, which strict
     * BIOS/UEFI boot-keyboard drivers treat as fatal. Reports are only
     * sent on change, so the requested rate is accepted and ignored.
     */
    LOG_DBG("%s: set idle id %u %u ms", dev->name, id, duration);
}

static void hid_set_protocol(const struct device* dev, const uint8_t proto) {
    /* Our report layouts are boot-compatible, nothing to switch. */
    LOG_INF("%s: %s protocol", dev->name, proto == 0U ? "boot" : "report");
}

static const struct hid_device_ops hid_ops = {
    .iface_ready = hid_iface_ready,
    .get_report = hid_get_report,
    .set_report = hid_set_report,
    .set_idle = hid_set_idle,
    .set_protocol = hid_set_protocol,
    .input_report_done = hid_input_report_done,
};

/* Public API */

int hid_out_init(void) {
    int ret;

    for (size_t i = 0; i < ARRAY_SIZE(chans); i++) {
        struct hid_chan* ch = &chans[i];

        if (!device_is_ready(ch->dev)) {
            LOG_ERR("%s not ready", ch->dev->name);
            return -ENODEV;
        }

        ret = hid_device_register(ch->dev, ch->rdesc, ch->rdesc_len, &hid_ops);
        if (ret != 0) {
            LOG_ERR("%s: register failed (%d)", ch->dev->name, ret);
            return ret;
        }
    }

    return 0;
}

void hid_out_keyboard(const uint8_t report[HID_KB_REPORT_LEN]) {
    if (usb_dev_is_suspended()) {
        usb_dev_wakeup();
    }

    chan_put(&chans[CHAN_KBD], report);
}

/* The descriptor's logical range is -127..127; -128 is not representable. */
static uint8_t clamp_rel(int8_t v) { return (uint8_t)MAX(v, -127); }

void hid_out_mouse_rel(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel) {
    uint8_t report[MOUSE_REL_REPORT_LEN] = {
        buttons & 0x07,
        clamp_rel(dx),
        clamp_rel(dy),
        clamp_rel(wheel),
    };

    chan_put(&chans[CHAN_MOUSE_REL], report);
}

/*
 * Last absolute position, so a reset can release buttons without moving the
 * pointer. Only touched from the protocol thread.
 */
static uint16_t abs_x;
static uint16_t abs_y;
static bool abs_used;

void hid_out_mouse_abs(uint8_t buttons, uint16_t x, uint16_t y, int8_t wheel) {
    uint8_t report[MOUSE_ABS_REPORT_LEN];

    x = MIN(x, MOUSE_ABS_MAX);
    y = MIN(y, MOUSE_ABS_MAX);
    abs_x = x;
    abs_y = y;
    abs_used = true;

    report[0] = buttons & 0x07;
    sys_put_le16(x, &report[1]);
    sys_put_le16(y, &report[3]);
    report[5] = clamp_rel(wheel);

    chan_put(&chans[CHAN_MOUSE_ABS], report);
}

void hid_out_release_all(void) {
    static const uint8_t zeros[HID_MAX_REPORT_LEN];

    chan_put(&chans[CHAN_KBD], zeros);
    chan_put(&chans[CHAN_MOUSE_REL], zeros);

    /* An all-zero absolute report would warp the pointer to (0, 0). */
    if (abs_used) {
        hid_out_mouse_abs(0, abs_x, abs_y, 0);
    }
}
