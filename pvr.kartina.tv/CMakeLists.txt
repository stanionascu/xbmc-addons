project(XBMC_KartinaTV)
cmake_minimum_required(VERSION 2.8)

if(NOT DEFINED XbmcPvrAddons_DIR)
    message(FATAL_ERROR "Cannot continue without XbmcPvrAddons_DIR")
endif()

include_directories("${XbmcPvrAddons_DIR}/xbmc"
    "${XbmcPvrAddons_DIR}/lib"
    "${XbmcPvrAddons_DIR}/lib/jsoncpp/include"
)

if(MSVC)
    set(CMAKE_CXX_FLAGS "/EHsc")
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS} /MTd")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS} /MT")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS}")

    add_definitions(-D_WINDLL
        -DTARGET_WINDOWS
        -D_CRT_SECURE_NO_WARNINGS
        -D_USE_32BIT_TIME_T
        -D_MBCS
    )
endif()

if(WIN32)
    include_directories(APPEND
        "${XbmcPvrAddons_DIR}/lib/platform/windows"
    )
endif()

file(GLOB_RECURSE SRC_LIST "./src/" "./src/*")

configure_file("./src/addons/addon.xml.in" "./src/addons/addon.xml")

add_library(${PROJECT_NAME} SHARED ${SRC_LIST})

if(WIN32)
    if(MSVC)
        target_link_libraries(${PROJECT_NAME}
            ${XbmcPvrAddons_DIR}/project/VS2010Express/${CMAKE_BUILD_TYPE}/platform.lib
            ${XbmcPvrAddons_DIR}/project/VS2010Express/${CMAKE_BUILD_TYPE}/libjsoncpp.lib
            ws2_32
        )
    else()
        message(FATAL_ERROR "Update here for different setups")
    endif()
    set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "XBMC_KartinaTV_win32")
endif()

install(TARGETS ${PROJECT_NAME} DESTINATION "lib/xbmc/addons/pvr.kartina.tv")
install(FILES "${CMAKE_BINARY_DIR}/src/addons/addon.xml" DESTINATION "lib/xbmc/addons/pvr.kartina.tv")
install(FILES "${CMAKE_SOURCE_DIR}/src/addons/resources/settings.xml" DESTINATION "lib/xbmc/addons/pvr.kartina.tv/resources")
