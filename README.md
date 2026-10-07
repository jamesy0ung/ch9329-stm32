# ch9329-stm32

Make your nucleo_u575zi_q pretend to be a CH9329

This is useful as the STM32 has a built in ST-Link which does USB to Serial and then the STM32 parses this and turns it into HID events.

This code should be pretty portable to other boards supporting Zephyr too such as the RP2040/2350.
