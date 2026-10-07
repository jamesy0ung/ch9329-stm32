cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ch9329_stm32)

target_sources(app PRIVATE
	src/main.c
	src/ch9329_proto.c
	src/hid_out.c
	src/usb_dev.c
)
