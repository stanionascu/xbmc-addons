project(XBMC_KartinaTV)
cmake_minimum_required(VERSION 2.8)

set(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake/modules")

if(NOT DEFINED XbmcPvrAddons_DIR)
    message(FATAL_ERROR "Cannot continue without XbmcPvrAddons_DIR")
endif()

find_package(LibCURL REQUIRED)

include_directories("${XbmcPvrAddons_DIR}/xbmc"
    "${XbmcPvrAddons_DIR}/lib"
    "${XbmcPvrAddons_DIR}/lib/jsoncpp/include"
    "${LIBCURL_INCLUDE_DIR}"
)

if(MSVC)
    set(CMAKE_CXX_FLAGS "/EHsc")
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS} /MTd")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS} /MT")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS}")

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
    include_directories(APPEND
        "${XbmcPvrAddons_DIR}/lib/platform/windows"
    )
endif()

file(GLOB_RECURSE SRC_LIST "./src/" "./src/*")

configure_file("./src/addons/addon.xml.in" "./src/addons/addon.xml")

add_library(${PROJECT_NAME} SHARED ${SRC_LIST})
target_link_libraries(${PROJECT_NAME} ${LIBCURL_LIBRARIES})

if(WIN32)
    target_link_libraries(${PROJECT_NAME} ${XbmcPvrAddons_PLATFORM_LIBRARY} ws2_32)
    set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "XBMC_KartinaTV_win32")
endif()

install(TARGETS ${PROJECT_NAME} DESTINATION "lib/xbmc/addons/pvr.kartina.tv")
install(FILES "${CMAKE_BINARY_DIR}/src/addons/addon.xml" DESTINATION "lib/xbmc/addons/pvr.kartina.tv")
install(FILES "${CMAKE_SOURCE_DIR}/src/addons/resources/settings.xml" DESTINATION "lib/xbmc/addons/pvr.kartina.tv/resources")
