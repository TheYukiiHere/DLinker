cmake_minimum_required(VERSION 3.20)

project(DLinkerSDL3
    VERSION 0.1.0
    LANGUAGES CXX
)

option(DLINKER_BUILD_CLIENT "Build the SDL3 Windows client" ON)

if (DLINKER_BUILD_CLIENT)
    include(FetchContent)

    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)

    FetchContent_Declare(
        SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG release-3.4.18
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(SDL3)

    add_executable(${PROJECT_NAME}
        src/main.cpp
    )

    target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_17)
    target_link_libraries(${PROJECT_NAME} PRIVATE SDL3::SDL3)

    if (WIN32)
        target_link_libraries(${PROJECT_NAME} PRIVATE ws2_32)
    endif()

    if (WIN32)
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:SDL3::SDL3>"
                "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
        )
    endif()
endif()

if (UNIX)
    find_package(X11)
    find_package(PkgConfig)
    if (PkgConfig_FOUND)
        pkg_check_modules(ALSA alsa)
    endif()
    add_executable(flink-agent
        src/flink_agent.cpp
    )
    target_compile_features(flink-agent PRIVATE cxx_std_17)
    if (X11_FOUND)
        target_compile_definitions(flink-agent PRIVATE FLINK_HAS_X11=1)
        target_include_directories(flink-agent PRIVATE ${X11_INCLUDE_DIR})
        target_link_libraries(flink-agent PRIVATE ${X11_LIBRARIES})
    endif()
    find_library(XTEST_LIBRARY Xtst)
    if (XTEST_LIBRARY)
        target_compile_definitions(flink-agent PRIVATE FLINK_HAS_XTEST=1)
        target_link_libraries(flink-agent PRIVATE ${XTEST_LIBRARY})
    endif()
    if (ALSA_FOUND)
        target_compile_definitions(flink-agent PRIVATE FLINK_HAS_ALSA=1)
        target_include_directories(flink-agent PRIVATE ${ALSA_INCLUDE_DIRS})
        target_link_libraries(flink-agent PRIVATE ${ALSA_LIBRARIES})
    endif()
endif()
