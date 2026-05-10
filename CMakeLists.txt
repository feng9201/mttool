cmake_minimum_required(VERSION 3.15)
project(mttool VERSION 1.0.0 LANGUAGES NONE)

option(MT_TOOL_ENABLE_EVENTPP "Install eventpp headers" ON)

add_library(${PROJECT_NAME} INTERFACE)

target_include_directories(${PROJECT_NAME}
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(${PROJECT_NAME}
    INTERFACE
        cxx_std_17
)

target_compile_definitions(${PROJECT_NAME}
    INTERFACE
        MT_TOOL_LIB_AVAILABLE
        $<$<BOOL:${MT_TOOL_ENABLE_EVENTPP}>:MT_TOOL_EVENTPP_AVAILABLE>
)

include(CMakePackageConfigHelpers)

configure_package_config_file(
    cmake/mttoolConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/mttoolConfig.cmake"
    INSTALL_DESTINATION lib/cmake/${PROJECT_NAME}
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/mttoolConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/mttoolConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/mttoolConfigVersion.cmake"
    DESTINATION lib/cmake/${PROJECT_NAME}
)

install(TARGETS ${PROJECT_NAME}
    EXPORT ${PROJECT_NAME}Targets
    INCLUDES DESTINATION include
)

install(DIRECTORY include/mttool
    DESTINATION include
)

if(MT_TOOL_ENABLE_EVENTPP)
    install(DIRECTORY include/eventpp
        DESTINATION include
    )
endif()

install(EXPORT ${PROJECT_NAME}Targets
    FILE ${PROJECT_NAME}Targets.cmake
    NAMESPACE ${PROJECT_NAME}::
    DESTINATION lib/cmake/${PROJECT_NAME}
)