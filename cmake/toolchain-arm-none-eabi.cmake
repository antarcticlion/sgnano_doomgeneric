# sgnano の SGX を arm-none-eabi-gcc で組むための道具立て（2026-11-17）。
# sgnano 本体は Pico SDK の toolchain を使うが、DOOM.SGX はファームと結ばないので SDK は要らない。
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_C_COMPILER arm-none-eabi-gcc)
set(CMAKE_ASM_COMPILER arm-none-eabi-gcc)
set(CMAKE_OBJCOPY arm-none-eabi-objcopy)
set(CMAKE_SIZE arm-none-eabi-size)
set(CMAKE_AR arm-none-eabi-ar)
set(CMAKE_RANLIB arm-none-eabi-ranlib)

# sgnano のファーム（RP2350B、Cortex-M33）と同じ並び。浮動小数点は softfp（SGX の ABI）。
set(SGNANO_MCU_FLAGS "-mcpu=cortex-m33 -mthumb -march=armv8-m.main+fp+dsp -mfloat-abi=softfp -mcmse")

set(CMAKE_C_FLAGS_INIT   "${SGNANO_MCU_FLAGS} -g -O3 -DNDEBUG -Wall -Wextra")
set(CMAKE_ASM_FLAGS_INIT "${SGNANO_MCU_FLAGS} -g")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${SGNANO_MCU_FLAGS}")
