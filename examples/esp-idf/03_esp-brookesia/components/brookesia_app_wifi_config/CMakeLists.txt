set(PROJ_SRC ${CMAKE_CURRENT_LIST_DIR})

file(GLOB_RECURSE PROJ_SRCS_C ${PROJ_SRC}/*.c)
file(GLOB_RECURSE PROJ_SRCS_CPP ${PROJ_SRC}/*.cpp)

# WHOLE_ARCHIVE 必须保留:注册宏是静态构造器,链接器裁剪后 app 不会进 registry
idf_component_register(
    SRCS  ${PROJ_SRCS_C} ${PROJ_SRCS_CPP}
    INCLUDE_DIRS ${PROJ_SRC}
    PRIV_REQUIRES esp_wifi nvs_flash esp_netif esp_event
    WHOLE_ARCHIVE
)

set_source_files_properties(
    ${PROJ_SRCS_CPP}
    PROPERTIES
        COMPILE_FLAGS "-Wno-missing-field-initializers"
)
