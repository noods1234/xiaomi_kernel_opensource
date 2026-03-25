# CMakeLists.txt — Rev A-1 eND Controller Firmware
# Target: STM32U5A5RJT6 (Cortex-M33, 2MB Flash, 786KB SRAM)
# Toolchain: arm-none-eabi-gcc
# Document: LCDRV-A1-FW-001 Rev A  |  2026-03-25
#
# Prerequisites:
#   STM32CUBEU5_ROOT — path to STM32CubeU5 SDK root (set in env or cmake -D)
#   arm-none-eabi-gcc in PATH
#
# Build:
#   mkdir build && cd build
#   cmake .. -DCMAKE_TOOLCHAIN_FILE=../cmake/arm-none-eabi.cmake
#   make -j$(nproc)
#   arm-none-eabi-objcopy -O binary nd_controller_a1.elf nd_controller_a1.bin

cmake_minimum_required(VERSION 3.22)

project(nd_controller_a1 C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)

# ── Toolchain flags ───────────────────────────────────────────── #
set(MCU_FLAGS
    "-mcpu=cortex-m33"
    "-mthumb"
    "-mfpu=fpv5-sp-d16"
    "-mfloat-abi=hard"
)

add_compile_options(
    ${MCU_FLAGS}
    -Wall -Wextra
    -fdata-sections -ffunction-sections
    -fno-exceptions
    -Os
)

add_link_options(
    ${MCU_FLAGS}
    -Wl,--gc-sections
    -Wl,-Map=${PROJECT_NAME}.map,--cref
    -T${CMAKE_SOURCE_DIR}/STM32U5A5RJT6_FLASH.ld
    -lm
    --specs=nano.specs
    --specs=nosys.specs
)

# ── STM32CubeU5 SDK ───────────────────────────────────────────── #
if(NOT DEFINED STM32CUBEU5_ROOT)
    if(DEFINED ENV{STM32CUBEU5_ROOT})
        set(STM32CUBEU5_ROOT $ENV{STM32CUBEU5_ROOT})
    else()
        message(FATAL_ERROR "STM32CUBEU5_ROOT not set")
    endif()
endif()

set(CUBE_INC
    ${STM32CUBEU5_ROOT}/Drivers/CMSIS/Device/ST/STM32U5xx/Include
    ${STM32CUBEU5_ROOT}/Drivers/CMSIS/Include
    ${STM32CUBEU5_ROOT}/Drivers/STM32U5xx_HAL_Driver/Inc
    ${STM32CUBEU5_ROOT}/Middlewares/ST/STM32_USB_Device_Library/Core/Inc
    ${STM32CUBEU5_ROOT}/Middlewares/ST/STM32_USB_Device_Library/Class/CDC/Inc
    ${STM32CUBEU5_ROOT}/Middlewares/Third_Party/FreeRTOS/Source/include
    ${STM32CUBEU5_ROOT}/Middlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM33_NTZ/non_secure
)

file(GLOB_RECURSE CUBE_HAL_SRC
    ${STM32CUBEU5_ROOT}/Drivers/STM32U5xx_HAL_Driver/Src/*.c
)
# Exclude template files
list(FILTER CUBE_HAL_SRC EXCLUDE REGEX ".*_template\\.c$")

file(GLOB_RECURSE CUBE_USB_SRC
    ${STM32CUBEU5_ROOT}/Middlewares/ST/STM32_USB_Device_Library/Core/Src/*.c
    ${STM32CUBEU5_ROOT}/Middlewares/ST/STM32_USB_Device_Library/Class/CDC/Src/*.c
)

file(GLOB_RECURSE CUBE_RTOS_SRC
    ${STM32CUBEU5_ROOT}/Middlewares/Third_Party/FreeRTOS/Source/*.c
    ${STM32CUBEU5_ROOT}/Middlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM33_NTZ/non_secure/*.c
    ${STM32CUBEU5_ROOT}/Middlewares/Third_Party/FreeRTOS/Source/portable/MemMang/heap_4.c
)

# ── Application sources ───────────────────────────────────────── #
set(APP_SRC
    Core/Src/main.c
    Core/Src/tmp117_driver.c
    Core/Src/ad5696r_driver.c
    Core/Src/lc_drive.c
    Core/Src/nd_control.c
    Core/Src/dc_monitor.c
    Core/Src/usb_cdc_handler.c
    Core/Src/lut.c
    Core/Src/pid.c
    Core/Src/syscalls.c
    Core/Src/sysmem.c
    Core/Startup/startup_stm32u5a5rjtx.s
)

include_directories(
    Core/Inc
    ${CUBE_INC}
)

add_compile_definitions(
    STM32U5A5xx
    USE_HAL_DRIVER
    USE_FULL_LL_DRIVER
)

add_executable(${PROJECT_NAME}.elf
    ${APP_SRC}
    ${CUBE_HAL_SRC}
    ${CUBE_USB_SRC}
    ${CUBE_RTOS_SRC}
)

# ── Post-build: generate .bin and print size ──────────────────── #
add_custom_command(TARGET ${PROJECT_NAME}.elf POST_BUILD
    COMMAND arm-none-eabi-objcopy -O binary
            ${PROJECT_NAME}.elf ${PROJECT_NAME}.bin
    COMMAND arm-none-eabi-size ${PROJECT_NAME}.elf
    COMMENT "Generating ${PROJECT_NAME}.bin"
)
