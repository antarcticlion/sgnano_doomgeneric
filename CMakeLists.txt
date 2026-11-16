cmake_minimum_required(VERSION 3.20)

# sgnano の外部実行ファイル DOOM.SGX を単体で組む（2026-11-17）。
# ねらい: doomgeneric（GPL-2.0）と sgnano のつなぎだけをこのリポジトリに置き、sgnano 本体とは
# SGX（docs/sgx_abi.md）の API の表だけでつながる形にする。
project(sgnano_doomgeneric VERSION 0.1.0 LANGUAGES C ASM)

if(NOT CMAKE_C_COMPILER_ID STREQUAL "GNU")
    message(FATAL_ERROR "arm-none-eabi-gcc (GNU) が要ります（cmake/toolchain-arm-none-eabi.cmake を使ってください）")
endif()

if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^arm")
    message(FATAL_ERROR
        "クロスコンパイルの設定がありません。次のどちらかで組んでください:\n"
        "  cmake --preset sgx\n"
        "  cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-arm-none-eabi.cmake")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

set(SGX_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/sdk/sgx")
set(DOOM_PORT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/apps/doom")
set(DOOMGENERIC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/doomgeneric")

include("${SGX_SDK_DIR}/sgx.cmake")

# doomgeneric: 取り込んだソース。画面（i_video.c）と始め方（doomgeneric.c）は apps/doom の自前のものを使う。
file(GLOB DOOMGENERIC_SOURCES CONFIGURE_DEPENDS "${DOOMGENERIC_DIR}/*.c")
list(FILTER DOOMGENERIC_SOURCES EXCLUDE REGEX "/(doomgeneric|i_video)\\.c$")

set(DOOM_PORT_SOURCES
    ${DOOM_PORT_DIR}/doom_app.c
    ${DOOM_PORT_DIR}/dg_sys.c
    ${DOOM_PORT_DIR}/dg_video.c
    ${DOOM_PORT_DIR}/dg_sound.c
    ${DOOM_PORT_DIR}/dg_opl.c)

# SGX のプログラム。画面モード 7（320x200x8）、スタック 16KB、ESC をアプリへ渡す（SGX_F_ESC_KEY）。
sgx_add_program(doom NAME "Doom" FLAGS 1 STACK 16384
    SOURCES ${DOOMGENERIC_SOURCES} ${DOOM_PORT_SOURCES}
    INCLUDES ${DOOMGENERIC_DIR} ${DOOM_PORT_DIR}
    DEFINES CMAP256 DOOMGENERIC_RESX=320 DOOMGENERIC_RESY=200 SGNANO FEATURE_SOUND)

# 取り込んだソースだけに libc の口の向け直し（dg_port.h）を読ませる。警告は出さない（取り込んだまま）。
set_source_files_properties(${DOOMGENERIC_SOURCES} ${DOOM_PORT_DIR}/dg_video.c ${DOOM_PORT_DIR}/dg_sound.c PROPERTIES
    COMPILE_OPTIONS "-include;${DOOM_PORT_DIR}/dg_port.h;-w")

# こちらのコードは警告を見る（警告で止めるかは DOOM_WERROR で選ぶ）。
option(DOOM_WERROR "treat warnings in apps/doom as errors" OFF)
target_compile_options(doom PRIVATE -Wall -Wextra)
if(DOOM_WERROR)
    target_compile_options(doom PRIVATE -Werror)
endif()

message(STATUS "sgnano_doomgeneric: 出力は ${CMAKE_BINARY_DIR}/sgx/doom.sgx")
