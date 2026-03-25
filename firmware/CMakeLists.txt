# SPDX-License-Identifier: GPL-2.0-only
# CMakeLists.txt — LCDRV-001 STM32G0B1 Firmware
# ONE INCH WONDER — LC Variable ND Filter Driver
# Document: LCDRV-FW-001 Rev A  |  2026-03-24
#
# Prerequisites
# ─────────────
# 1. Install STM32CubeG0 SDK (HAL + FreeRTOS + USB Device middleware):
#      https://github.com/STMicroelectronics/STM32CubeG0
#    Set env variable: STM32CUBEG0_ROOT=/path/to/STM32CubeG0
#
# 2. Generate the base project with STM32CubeMX (or CubeIDE) for
#    STM32G0B1KET6 with the following options:
#      USB:   CDC device, FS
#      SPI1:  Master, Mode 1, 10 MHz, 8-bit, software NSS
#      ADC1:  CH0 (PA0) + CH1 (PA1), 12-bit, software trigger
#      RTOS:  FreeRTOS, native API
#    Copy the generated startup_stm32g0b1xx.s, stm32g0xx_hal_msp.c,
#    STM32G0B1KETX_FLASH.ld, and the USB/FreeRTOS middleware to this
#    project tree.
#
# 3. Toolchain:
#      arm-none-eabi-gcc >= 12.x
#      cmake >= 3.22
#
# Usage:
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake
#   cmake --build build
#   # Flash via DFU (BOOT0 high + NRST):
#   dfu-util -a 0 -s 0x08000000:leave -D build/nd_controller.bin

cmake_minimum_required(VERSION 3.22)
project(nd_controller C ASM)

set(CMAKE_C_STANDARD 11)
set(MCU_FLAGS "-mcpu=cortex-m0plus -mthumb -mfloat-abi=soft")

# ── STM32CubeG0 paths (set STM32CUBEG0_ROOT in environment) ───── #
set(CUBE_ROOT "$ENV{STM32CUBEG0_ROOT}"
    CACHE PATH "Root of STM32CubeG0 repository")

set(HAL_SRC  ${CUBE_ROOT}/Drivers/STM32G0xx_HAL_Driver/Src)
set(CMSIS    ${CUBE_ROOT}/Drivers/CMSIS)
set(RTOS_SRC ${CUBE_ROOT}/Middlewares/Third_Party/FreeRTOS/Source)
set(USB_SRC  ${CUBE_ROOT}/Middlewares/ST/STM32_USB_Device_Library)

# ── Application sources ─────────────────────────────────────────  #
set(APP_SOURCES
    Core/Src/main.c
    Core/Src/usb_cdc_handler.c
    Core/Src/ad9833_driver.c
    Core/Src/mcp4131_driver.c
    Core/Src/lut.c
    Core/Src/temp_sense.c
    Core/Src/dc_monitor.c
    Core/Src/nd_control.c
    Core/Src/pid.c
)

# ── HAL driver sources (subset — add others as required by CubeMX) #
set(HAL_SOURCES
    ${HAL_SRC}/stm32g0xx_hal.c
    ${HAL_SRC}/stm32g0xx_hal_cortex.c
    ${HAL_SRC}/stm32g0xx_hal_rcc.c
    ${HAL_SRC}/stm32g0xx_hal_gpio.c
    ${HAL_SRC}/stm32g0xx_hal_spi.c
    ${HAL_SRC}/stm32g0xx_hal_adc.c
    ${HAL_SRC}/stm32g0xx_hal_adc_ex.c
    ${HAL_SRC}/stm32g0xx_hal_flash.c
    ${HAL_SRC}/stm32g0xx_hal_flash_ex.c
    ${HAL_SRC}/stm32g0xx_hal_pwr.c
    ${HAL_SRC}/stm32g0xx_hal_pwr_ex.c
    ${HAL_SRC}/stm32g0xx_hal_tim.c
    ${HAL_SRC}/stm32g0xx_hal_tim_ex.c
)

# ── FreeRTOS kernel sources ─────────────────────────────────────── #
set(RTOS_SOURCES
    ${RTOS_SRC}/tasks.c
    ${RTOS_SRC}/queue.c
    ${RTOS_SRC}/list.c
    ${RTOS_SRC}/timers.c
    ${RTOS_SRC}/portable/GCC/ARM_CM0/port.c
    ${RTOS_SRC}/portable/MemMang/heap_4.c
)

# ── USB CDC middleware sources ──────────────────────────────────── #
set(USB_SOURCES
    ${USB_SRC}/Core/Src/usbd_core.c
    ${USB_SRC}/Core/Src/usbd_ctlreq.c
    ${USB_SRC}/Core/Src/usbd_ioreq.c
    ${USB_SRC}/Class/CDC/Src/usbd_cdc.c
    # CubeMX-generated glue (copy to Core/Src after generation):
    Core/Src/usb_device.c
    Core/Src/usbd_conf.c
    Core/Src/usbd_desc.c
    Core/Src/usbd_cdc_if.c
    # Hook usb_cdc_on_rx() into CDC_Receive_FS() in usbd_cdc_if.c
)

# ── Startup + system files (from CubeMX generation) ────────────── #
set(STARTUP_SOURCES
    Core/Src/startup_stm32g0b1xx.s
    Core/Src/system_stm32g0xx.c
    Core/Src/stm32g0xx_hal_msp.c
    Core/Src/stm32g0xx_it.c
)

add_executable(${PROJECT_NAME}.elf
    ${APP_SOURCES}
    ${HAL_SOURCES}
    ${RTOS_SOURCES}
    ${USB_SOURCES}
    ${STARTUP_SOURCES}
)

target_include_directories(${PROJECT_NAME}.elf PRIVATE
    Core/Inc
    ${CMSIS}/Device/ST/STM32G0xx/Include
    ${CMSIS}/Include
    ${HAL_SRC}/../Inc
    ${RTOS_SRC}/include
    ${RTOS_SRC}/portable/GCC/ARM_CM0
    ${USB_SRC}/Core/Inc
    ${USB_SRC}/Class/CDC/Inc
)

target_compile_definitions(${PROJECT_NAME}.elf PRIVATE
    STM32G0B1xx
    USE_HAL_DRIVER
)

target_compile_options(${PROJECT_NAME}.elf PRIVATE
    ${MCU_FLAGS}
    -Wall -Wextra
    -ffunction-sections -fdata-sections
    -Os
)

target_link_options(${PROJECT_NAME}.elf PRIVATE
    ${MCU_FLAGS}
    -T${CMAKE_SOURCE_DIR}/STM32G0B1KETX_FLASH.ld
    -Wl,--gc-sections
    -Wl,-Map=${PROJECT_NAME}.map
    -lm          # math.h (logf for Steinhart-Hart)
    --specs=nano.specs
)

# ── Post-build: generate .bin for DFU flashing ─────────────────── #
add_custom_command(TARGET ${PROJECT_NAME}.elf POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary
            $<TARGET_FILE:${PROJECT_NAME}.elf>
            ${PROJECT_NAME}.bin
    COMMENT "Generating ${PROJECT_NAME}.bin"
)

# ── Flash target (requires dfu-util) ───────────────────────────── #
add_custom_target(flash
    COMMAND dfu-util -a 0 -s 0x08000000:leave -D ${PROJECT_NAME}.bin
    DEPENDS ${PROJECT_NAME}.elf
    COMMENT "Flashing via DFU (ensure BOOT0 is high)"
)
