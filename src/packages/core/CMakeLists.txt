file(GLOB PACKAGE_CORE_SOURCE "*.cc")
file(GLOB PACKAGE_CORE_HEADER "*.h")

add_library(package_core STATIC
        ${PACKAGE_CORE_SOURCE}
        ${PACKAGE_CORE_HEADER}
        )

find_package(ZLIB REQUIRED)
target_include_directories(package_core PRIVATE ${ZLIB_INCLUDE_DIRS})
target_link_libraries(package_core ${ZLIB_LIBRARIES})
