#ifndef USB_DEV_H_
#define USB_DEV_H_

#include <stdbool.h>

/*
 * Set up descriptors and configuration, register all HID class instances
 * and enable the device (immediately, or on VBUS detection if supported).
 * HID devices must be registered with hid_device_register() first.
 */
int usb_dev_init(void);

/* True if the host has suspended the bus. */
bool usb_dev_is_suspended(void);

/* Request remote wakeup if the host allows it. */
void usb_dev_wakeup(void);

#endif /* USB_DEV_H_ */
