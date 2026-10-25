# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)
# 全局 include:LVGL 组件通过 CONFIG_LV_ASSERT_HANDLER_INCLUDE 找到我们的断言处理头
# (见 main/lvgl_hooks/lv_assert_handler.h:把 while(1) 改成即时 panic)
include_directories(${CMAKE_CURRENT_LIST_DIR}/main/lvgl_hooks)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)

set(PROJECT_VER "2.5.0")
project(xiaozhi)
