project(XBMC_KartinaTV)
cmake_minimum_required(VERSION 2.8)

set(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake/modules")

if(NOT DEFINED XbmcAddons_DIR)
    message(FATAL_ERROR "Please provide XbmcAddons_DIR to continue")
endif()

find_package(LibJSONC REQUIRED)
find_package(LibCURL REQUIRED)

include_directories("${XbmcAddons_DIR}/include"
    "${XbmcAddons_DIR}/../../addons/library.xbmc.addon"
    "${XbmcAddons_DIR}/../../addons/library.xbmc.pvr"
    "${XbmcAddons_DIR}/../../addons/library.xbmc.gui"
    "${XbmcAddons_DIR}/.."
)

file(GLOB_RECURSE SRC_LIST "./src/" "./src/*")

configure_file("./src/addons/addon.xml.in" "./src/addons/addon.xml")

add_library(${PROJECT_NAME} SHARED ${SRC_LIST})
target_link_libraries(${PROJECT_NAME} ${LIBCURL_LIBRARIES} ${LIBJSONC_LIBRARIES})

install(TARGETS ${PROJECT_NAME} DESTINATION "lib/xbmc/addons/pvr.kartina.tv")
install(FILES "${CMAKE_BINARY_DIR}/src/addons/addon.xml" DESTINATION "lib/xbmc/addons/pvr.kartina.tv")
install(FILES "${CMAKE_SOURCE_DIR}/src/addons/resources/settings.xml" DESTINATION "lib/xbmc/addons/pvr.kartina.tv/resources")
