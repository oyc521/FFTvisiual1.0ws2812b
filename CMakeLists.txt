# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)
project(FFTvisiual1.0ws2812b VERSION 1.0.20)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/components")
# Components register themselves in their own CMakeLists under `components/`
# Keep `${EXTRA_COMPONENT_DIRS}` set, CMake will discover component CMake files automatically.
# Components: main, dual_core_com, wifi_core, led_core, audio_processor, led_controller, utils

# 依赖关系
# Components are discovered and linked automatically by ESP-IDF. Do not manually add
# `target_link_libraries(${PROJECT_NAME}.elf PRIVATE ...)` with component names here
# since that produces extra `-l<component>` flags which can break linking on Windows.
# If you need to add extra libraries, link against targets explicitly or adjust component CMake files.

# 可选：启用最小构建（如果需要最小化固件大小）
# idf_build_set_property(MINIMAL_BUILD ON)

# 配置组件（可选，可以通过menuconfig设置）
# ESP-IDF v5.x 会自动发现和链接组件，无需手动指定 COMPONENT_REQUIRES

# 设置编译器选项（针对音频处理优化）
add_compile_options(
    -O2
    -ffast-math
    -fno-finite-math-only
)

# 设置链接器选项
add_link_options(
    -Wl,--gc-sections
    -Wl,--cref
)