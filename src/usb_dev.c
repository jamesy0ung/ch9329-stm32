/*
 * USB device context, descriptors and enable/VBUS handling.
 * The STM32U575 OTG_FS controller is full-speed only.
 */

#include "usb_dev.h"

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/usb/usbd.h>

LOG_MODULE_REGISTER(usb_dev, LOG_LEVEL_INF);

USBD_DEVICE_DEFINE(app_usbd, DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),
                   CONFIG_APP_USBD_VID, CONFIG_APP_USBD_PID);

USBD_DESC_LANG_DEFINE(app_lang);
USBD_DESC_MANUFACTURER_DEFINE(app_mfr, CONFIG_APP_USBD_MANUFACTURER);
USBD_DESC_PRODUCT_DEFINE(app_product, CONFIG_APP_USBD_PRODUCT);
IF_ENABLED(CONFIG_HWINFO, (USBD_DESC_SERIAL_NUMBER_DEFINE(app_sn)));

USBD_DESC_CONFIG_DEFINE(fs_cfg_desc, "FS Configuration");

/* Powered from the ST-Link USB (CN1), not from the target on CN15. */
static const uint8_t attributes =
    USB_SCD_SELF_POWERED |
    (IS_ENABLED(CONFIG_APP_USBD_REMOTE_WAKEUP) ? USB_SCD_REMOTE_WAKEUP : 0);

USBD_CONFIGURATION_DEFINE(app_fs_config, attributes, 50, &fs_cfg_desc);

static void msg_cb(struct usbd_context* const usbd_ctx,
                   const struct usbd_msg* const msg) {
    LOG_INF("USBD message: %s", usbd_msg_type_string(msg->type));

    if (usbd_can_detect_vbus(usbd_ctx)) {
        if (msg->type == USBD_MSG_VBUS_READY) {
            if (usbd_enable(usbd_ctx)) {
                LOG_ERR("Failed to enable device support");
            }
        }

        if (msg->type == USBD_MSG_VBUS_REMOVED) {
            if (usbd_disable(usbd_ctx)) {
                LOG_ERR("Failed to disable device support");
            }
        }
    }
}

int usb_dev_init(void) {
    int err;

    err = usbd_add_descriptor(&app_usbd, &app_lang);
    if (err == 0) {
        err = usbd_add_descriptor(&app_usbd, &app_mfr);
    }
    if (err == 0) {
        err = usbd_add_descriptor(&app_usbd, &app_product);
    }
    IF_ENABLED(CONFIG_HWINFO, (if (err == 0) {
                   err = usbd_add_descriptor(&app_usbd, &app_sn);
               }))
    if (err) {
        LOG_ERR("Failed to add string descriptors (%d)", err);
        return err;
    }

    err = usbd_add_configuration(&app_usbd, USBD_SPEED_FS, &app_fs_config);
    if (err) {
        LOG_ERR("Failed to add configuration (%d)", err);
        return err;
    }

    err = usbd_register_all_classes(&app_usbd, USBD_SPEED_FS, 1, NULL);
    if (err) {
        LOG_ERR("Failed to register classes (%d)", err);
        return err;
    }

    /* Composite of plain HID interfaces: class info is per interface. */
    usbd_device_set_code_triple(&app_usbd, USBD_SPEED_FS, 0, 0, 0);
    usbd_self_powered(&app_usbd, true);

    err = usbd_msg_register_cb(&app_usbd, msg_cb);
    if (err) {
        LOG_ERR("Failed to register message callback (%d)", err);
        return err;
    }

    err = usbd_init(&app_usbd);
    if (err) {
        LOG_ERR("Failed to initialize device support (%d)", err);
        return err;
    }

    if (!usbd_can_detect_vbus(&app_usbd)) {
        err = usbd_enable(&app_usbd);
        if (err) {
            LOG_ERR("Failed to enable device support (%d)", err);
            return err;
        }
    }

    return 0;
}

bool usb_dev_is_suspended(void) { return usbd_is_suspended(&app_usbd); }

void usb_dev_wakeup(void) {
    int err;

    if (!IS_ENABLED(CONFIG_APP_USBD_REMOTE_WAKEUP)) {
        return;
    }

    err = usbd_wakeup_request(&app_usbd);
    if (err) {
        LOG_DBG("Remote wakeup not possible (%d)", err);
    }
}
