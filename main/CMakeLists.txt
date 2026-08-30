# main组件的CMakeLists.txt
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
)
# ESP-IDF v5.x 会自动发现并链接 components/ 下的所有组件