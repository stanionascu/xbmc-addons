project(XBMC_KartinaTV)
cmake_minimum_required(VERSION 2.8)

set(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake/modules")

if(NOT DEFINED XbmcAddons_DIR)
    message(FATAL_ERROR "Cannot continue without XbmcAddons_DIR")
endif()

find_package(LibJSONC REQUIRED)
find_package(LibCURL REQUIRED)

include_directories("${XbmcAddons_DIR}/include"
    "${XbmcAddons_DIR}/../../addons/library.xbmc.addon"
    "${XbmcAddons_DIR}/../../addons/library.xbmc.pvr"
    "${XbmcAddons_DIR}/../../addons/library.xbmc.gui"
    "${XbmcAddons_DIR}/.."
    "${LIBCURL_INCLUDE_DIR}"
    "${LIBJSONC_INCLUDE_DIR}"
)

if(MSVC)
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /MT")

    add_definitions(-D_WINDLL
        -DTARGET_WINDOWS
        -DCURL_STATICLIB
        -D_CRT_SECURE_NO_WARNINGS
        -D_USE_32BIT_TIME_T
        -D_MBCS
    )
endif()

if(WIN32)
    if(NOT DEFINED XbmcPvrAddons_PLATFORM_LIBRARY)
        message(FATAL_ERROR "Cannot continue without XbmcPvrAddons_PLATFORM_LIBRARY")
    endif()
    include_directories(AFTER
        "${XbmcAddons_DIR}/../win32"
    )
endif()

file(GLOB_RECURSE SRC_LIST "./src/" "./src/*")

configure_file("./src/addons/addon.xml.in" "./src/addons/addon.xml")

add_library(${PROJECT_NAME} SHARED ${SRC_LIST})
target_link_libraries(${PROJECT_NAME} ${LIBCURL_LIBRARIES} ${LIBJSONC_LIBRARIES})

if(WIN32)
    target_link_libraries(${PROJECT_NAME} ${XbmcPvrAddons_PLATFORM_LIBRARY} ws2_32)
    set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "XBMC_KartinaTV_win32")
endif()

install(TARGETS ${PROJECT_NAME} DESTINATION "lib/xbmc/addons/pvr.kartina.tv")
install(FILES "${CMAKE_BINARY_DIR}/src/addons/addon.xml" DESTINATION "lib/xbmc/addons/pvr.kartina.tv")
install(FILES "${CMAKE_SOURCE_DIR}/src/addons/resources/settings.xml" DESTINATION "lib/xbmc/addons/pvr.kartina.tv/resources")
