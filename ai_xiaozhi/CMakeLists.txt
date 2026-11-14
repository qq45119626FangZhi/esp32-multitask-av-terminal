# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# VERSION 会写进 esp_app_desc.version（OTA 列表/设备详情页版本号都以它为准）：
#   详情页显示 V1.0（取前两段），/api/status 返回完整 "1.0.0"
# 打包不同版本的 OTA 包时用 -DXZ_OTA_VER=x.y.z 覆盖，不需要改本文件：
#   idf.py -DXZ_OTA_VER=2.0.0 build   → build/atguigu_aixiaozhi.bin 就是 V2.0 的包
# 注意：该值会缓存在 build/CMakeCache.txt，之后不加 -D 也仍是上次的值，
#       要回到默认 1.0.0 需再执行一次 idf.py -DXZ_OTA_VER=1.0.0 build。
if(NOT DEFINED XZ_OTA_VER)
    set(XZ_OTA_VER "1.0.0")
endif()
project(atguigu_aixiaozhi VERSION ${XZ_OTA_VER})

# ---------------------------------------------------------------------------
# M2 摄像头：让 espressif/esp32-camera 使用新版 I2C 驱动（sccb-ng.c）
#
# 本板 OV2640 的 SCCB（SIOD/SIOC）与 ES8311 共用 GPIO0/1，而音频链路用的是
# driver/i2c_master 新驱动。esp32-camera 在 IDF < 5.4 时默认编译 driver/sccb.c
# （legacy I2C 驱动）：
#   1. legacy 驱动与本工程的新 I2C 驱动同时链接时，启动阶段会 abort
#      （CONFIG_I2C_SKIP_LEGACY_CONFLICT_CHECK 关闭时）；
#   2. 两套驱动无法同时占用同一组引脚，谁后初始化谁抢走引脚复用。
# sccb-ng.c 走新驱动并支持“复用已创建的 I2C 总线”
# （camera_config_t.pin_sccb_sda = -1 + sccb_i2c_port 指定端口号），
# 它用到的 i2c_master_get_bus_handle() 在 IDF 5.3 中已存在，因此这里直接换源。
# 若将来升级到 IDF >= 5.4，组件会自己选择 sccb-ng.c，本块自动失效（幂等）。
# ---------------------------------------------------------------------------
if(TARGET __idf_espressif__esp32-camera)
    set(_xz_cam_lib __idf_espressif__esp32-camera)
    set(_xz_cam_dir "${CMAKE_SOURCE_DIR}/managed_components/espressif__esp32-camera")
    get_target_property(_xz_cam_srcs ${_xz_cam_lib} SOURCES)
    if(_xz_cam_srcs)
        list(FILTER _xz_cam_srcs EXCLUDE REGEX "sccb\\.c$")
        set_property(TARGET ${_xz_cam_lib} PROPERTY SOURCES "${_xz_cam_srcs}")
        target_sources(${_xz_cam_lib} PRIVATE "${_xz_cam_dir}/driver/sccb-ng.c")
        # sccb-ng.c 会 include IDF 5.4 才有的私有头 esp_private/i2c_platform.h，
        # 5.3 下用本工程提供的空占位头（该文件实际未使用其中任何符号）。
        target_include_directories(${_xz_cam_lib} PRIVATE "${CMAKE_SOURCE_DIR}/main/camera/compat")
        message(STATUS "M2: esp32-camera 使用 sccb-ng.c（复用工程 I2C 总线）")
    endif()
endif()
