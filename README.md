# ch9329-stm32

Zephyr firmware that turns an STM32 **NUCLEO-U575ZI-Q** into a drop-in
replacement for a **WCH CH340 + CH9329** serial-to-USB-HID adapter
```
KVM host ──USB──► ST-Link VCP (CN1) ──USART1──► STM32U575 ──OTG FS (CN15)──► target PC
              CH9329 serial protocol                       keyboard + 2 mice (USB HID)
```