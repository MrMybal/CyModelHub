# SPDX-FileCopyrightText: 2026 Cyberalien
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.24)
project(CyModelHub VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32 AND NOT APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "CyModelHub targets Windows, Linux and macOS.")
endif()

# An application that embeds CyModelHub with add_subdirectory only gets the client library.
option(CYMODELHUB_BUILD_SERVICE "Build the local service, the MCP relay and the command line tool" ${PROJECT_IS_TOP_LEVEL})
option(CYMODELHUB_BUILD_APP "Build the CyUI desktop application" ${PROJECT_IS_TOP_LEVEL})
option(CYMODELHUB_BUILD_TESTS "Build the tests" ${PROJECT_IS_TOP_LEVEL})
if(CYMODELHUB_BUILD_TESTS AND NOT CYMODELHUB_BUILD_SERVICE)
    message(FATAL_ERROR "CYMODELHUB_BUILD_TESTS requires CYMODELHUB_BUILD_SERVICE.")
endif()

include(FetchContent)
if(APPLE)
    include(CheckCXXSourceCompiles)
    set(thread_check "#include <thread>\nint main() { std::jthread worker([](std::stop_token) {}); }")
    set(CMAKE_REQUIRED_FLAGS "-std=c++20")
    check_cxx_source_compiles("${thread_check}" CYMODELHUB_HAS_JTHREAD)
    if(NOT CYMODELHUB_HAS_JTHREAD)
        string(APPEND CMAKE_REQUIRED_FLAGS " -fexperimental-library")
        check_cxx_source_compiles("${thread_check}" CYMODELHUB_HAS_EXPERIMENTAL_JTHREAD)
        if(NOT CYMODELHUB_HAS_EXPERIMENTAL_JTHREAD)
            message(FATAL_ERROR "Use a recent Xcode toolchain with std::jthread support.")
        endif()
        add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-fexperimental-library>")
        add_link_options("$<$<LINK_LANGUAGE:CXX>:-fexperimental-library>")
    endif()
    unset(CMAKE_REQUIRED_FLAGS)
endif()

# JSON for the client library, whose public header uses nlohmann/json. A host project that
# already defines nlohmann_json::nlohmann_json before adding this directory keeps its own
# copy; otherwise the one in third_party is used. Nothing is downloaded for the client.
add_library(cymodelhub_json INTERFACE)
if(TARGET nlohmann_json::nlohmann_json)
    target_link_libraries(cymodelhub_json INTERFACE nlohmann_json::nlohmann_json)
else()
    target_include_directories(cymodelhub_json SYSTEM INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
endif()
find_package(Threads REQUIRED)

# Pinned dependencies of the service and the application. Point FETCHCONTENT_SOURCE_DIR_CYMCPSDK,
# _CYUI or _SDL3 at an existing checkout to build without network access.
if(CYMODELHUB_BUILD_SERVICE)
    if(NOT TARGET CyMCP::MCP)
        set(CYMCP_BUILD_HTTP ON CACHE BOOL "" FORCE)
        set(CYMCP_BUILD_PLATFORM ON CACHE BOOL "" FORCE)
        set(CYMCP_BUILD_MCP ON CACHE BOOL "" FORCE)
        set(CYMCP_BUILD_AI OFF CACHE BOOL "" FORCE)
        set(CYMCP_BUILD_LOCAL OFF CACHE BOOL "" FORCE)
        set(CYMCP_BUILD_CHAT_UI OFF CACHE BOOL "" FORCE)
        set(CYMCP_BUILD_CHAT_APP OFF CACHE BOOL "" FORCE)
        set(CYMCP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(CYMCP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(CyMCPSDK GIT_REPOSITORY https://github.com/MrMybal/CyMCPSDK.git
            GIT_TAG e562c731a146d599e3404718ea8414bb5b96b66d)
        FetchContent_MakeAvailable(CyMCPSDK)
    endif()
    # The service passes JSON values between this project and CyMCPSDK: both copies of
    # nlohmann/json must be the same release.
    if(DEFINED cymcpsdk_SOURCE_DIR AND NOT TARGET nlohmann_json::nlohmann_json)
        set(json_version "#define NLOHMANN_JSON_VERSION_(MAJOR|MINOR|PATCH) +[0-9]+")
        file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann/json.hpp" own_json REGEX "${json_version}")
        file(STRINGS "${cymcpsdk_SOURCE_DIR}/third_party/nlohmann/json.hpp" sdk_json REGEX "${json_version}")
        if(NOT own_json STREQUAL sdk_json)
            message(FATAL_ERROR "third_party/nlohmann/json.hpp and the copy shipped with CyMCPSDK are "
                "different releases. Update third_party/nlohmann together with the CyMCPSDK pin.")
        endif()
    endif()
endif()

function(cymodelhub_configure target)
    target_compile_features(${target} PUBLIC cxx_std_20)
    set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_compile_definitions(${target} PRIVATE CYMODELHUB_VERSION="${PROJECT_VERSION}")
    if(WIN32)
        target_compile_definitions(${target} PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
    if(MSVC)
        target_compile_options(${target} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/utf-8;/W4;/permissive-;/EHsc>")
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

# Licensing: the client library and everything compiled into it (include/cymodelhub/client.hpp,
# src/client.cpp, src/platform.*, third_party/nlohmann) are MIT, like this file, so any
# application may link them. The service, the application, the CLI and the MCP relay are
# GPL-3.0-only. Keep GPL sources out of the two targets below, and keep them self-contained:
# the C++ standard library, the operating system and nlohmann/json, nothing else.

# Paths, private files, the local HTTP exchange and process helpers shared by the client and
# the service.
add_library(modelhub_platform STATIC src/platform.cpp)
target_link_libraries(modelhub_platform PUBLIC cymodelhub_json Threads::Threads)
if(WIN32)
    target_link_libraries(modelhub_platform PRIVATE shell32 advapi32 ws2_32)
endif()
cymodelhub_configure(modelhub_platform)

# Client library: what an application links to find and manage the shared models.
add_library(cymodelhub_client STATIC src/client.cpp)
add_library(CyModelHub::Client ALIAS cymodelhub_client)
target_include_directories(cymodelhub_client PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(cymodelhub_client PUBLIC cymodelhub_json PRIVATE modelhub_platform)
cymodelhub_configure(cymodelhub_client)

if(CYMODELHUB_BUILD_SERVICE)
    add_library(modelhub_core STATIC src/model.cpp src/formats.cpp src/scanner.cpp src/catalog.cpp
        src/jobs.cpp src/install.cpp src/files.cpp src/package.cpp src/coordination.cpp src/installs.cpp
        src/service.cpp src/resident.cpp)
    target_link_libraries(modelhub_core PUBLIC CyMCP::MCP modelhub_platform)
    cymodelhub_configure(modelhub_core)

    add_executable(CyModelHubService src/service_main.cpp)
    target_link_libraries(CyModelHubService PRIVATE modelhub_core)

    add_executable(CyModelHubMcp src/mcp_main.cpp)
    target_link_libraries(CyModelHubMcp PRIVATE cymodelhub_client CyMCP::MCP modelhub_platform)

    add_executable(CyModelHubCli src/cli_main.cpp)
    target_link_libraries(CyModelHubCli PRIVATE cymodelhub_client modelhub_platform)

    foreach(target CyModelHubService CyModelHubMcp CyModelHubCli)
        cymodelhub_configure(${target})
    endforeach()
    install(TARGETS CyModelHubService CyModelHubMcp CyModelHubCli RUNTIME DESTINATION . COMPONENT Hub)
endif()

if(CYMODELHUB_BUILD_APP)
    if(NOT CYMODELHUB_BUILD_SERVICE)
        message(FATAL_ERROR "CYMODELHUB_BUILD_APP requires CYMODELHUB_BUILD_SERVICE.")
    endif()
    set(CYUI_MODULES "" CACHE STRING "" FORCE)
    set(CYUI_BUILD_DEMO OFF CACHE BOOL "" FORCE)
    set(CYUI_BUILD_AUDIO OFF CACHE BOOL "" FORCE)
    set(CYUI_BUILD_GALLERY OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(CyUI GIT_REPOSITORY https://github.com/MrMybal/CyUI.git
        GIT_TAG e8ccec0a96ed820fc46e86059e010efafcef687e)
    FetchContent_MakeAvailable(CyUI)

    find_package(SDL3 3.4 QUIET CONFIG)
    if(NOT TARGET SDL3::SDL3 AND NOT TARGET SDL3::SDL3-static)
        set(SDL_SHARED OFF CACHE BOOL "" FORCE)
        set(SDL_STATIC ON CACHE BOOL "" FORCE)
        set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
        set(SDL_TESTS OFF CACHE BOOL "" FORCE)
        set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(SDL3 GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
            GIT_TAG 683181b47cfabd293e3ea409f838915b8297a4fd)
        FetchContent_MakeAvailable(SDL3)
    endif()

    set(sdl_backends "${cyui_SOURCE_DIR}/third_party/imgui-sdl3")
    add_executable(CyModelHub WIN32 MACOSX_BUNDLE src/app_main.cpp src/app_ui.cpp src/app_data.cpp
        "${sdl_backends}/imgui_impl_sdl3.cpp" "${sdl_backends}/imgui_impl_sdlrenderer3.cpp")
    target_include_directories(CyModelHub PRIVATE "${sdl_backends}")
    target_link_libraries(CyModelHub PRIVATE cymodelhub_client modelhub_platform CyUI::Core)
    if(TARGET SDL3::SDL3-static)
        target_link_libraries(CyModelHub PRIVATE SDL3::SDL3-static)
    else()
        target_link_libraries(CyModelHub PRIVATE SDL3::SDL3)
    endif()
    cymodelhub_configure(CyModelHub)
    if(APPLE)
        set_target_properties(CyModelHub PROPERTIES MACOSX_BUNDLE_GUI_IDENTIFIER com.cyberalien.cymodelhub
            MACOSX_BUNDLE_BUNDLE_NAME CyModelHub MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
            MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}")
    endif()
    set(manual_dir "${CMAKE_CURRENT_SOURCE_DIR}/Documentation")
    add_custom_target(CyModelHub_manual
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${manual_dir}" "$<TARGET_FILE_DIR:CyModelHub>/Documentation"
        VERBATIM)
    add_dependencies(CyModelHub CyModelHubService CyModelHubMcp CyModelHub_manual)

    # Interface languages are plain JSON files next to the executable: add a file to add a language.
    add_custom_command(TARGET CyModelHub POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/locales"
            "$<TARGET_FILE_DIR:CyModelHub>/locales"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:CyModelHub>/licenses"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
            "$<TARGET_FILE_DIR:CyModelHub>/licenses/CyModelHub.txt"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE-MIT"
            "$<TARGET_FILE_DIR:CyModelHub>/licenses/CyModelHub-Client-MIT.txt"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${cyui_SOURCE_DIR}/LICENSE"
            "$<TARGET_FILE_DIR:CyModelHub>/licenses/CyUI.txt"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${cyui_SOURCE_DIR}/third_party/imgui/LICENSE.txt"
            "$<TARGET_FILE_DIR:CyModelHub>/licenses/Dear-ImGui.txt"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${cyui_SOURCE_DIR}/third_party/jetbrains-mono/OFL.txt"
            "$<TARGET_FILE_DIR:CyModelHub>/licenses/JetBrains-Mono.txt"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${cymcpsdk_SOURCE_DIR}/LICENSE"
            "$<TARGET_FILE_DIR:CyModelHub>/licenses/CyMCPSDK.txt"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann/LICENSE.MIT"
            "$<TARGET_FILE_DIR:CyModelHub>/licenses/nlohmann-json.txt"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${cymcpsdk_SOURCE_DIR}/third_party/httplib/LICENSE"
            "$<TARGET_FILE_DIR:CyModelHub>/licenses/cpp-httplib.txt"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${cymcpsdk_SOURCE_DIR}/third_party/picosha2/LICENSE"
            "$<TARGET_FILE_DIR:CyModelHub>/licenses/PicoSHA2.txt")
    if(DEFINED sdl3_SOURCE_DIR)
        add_custom_command(TARGET CyModelHub POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${sdl3_SOURCE_DIR}/LICENSE.txt" "$<TARGET_FILE_DIR:CyModelHub>/licenses/SDL.txt")
    endif()

    install(TARGETS CyModelHub RUNTIME DESTINATION . COMPONENT Hub BUNDLE DESTINATION . COMPONENT Hub)
    if(APPLE)
        install(DIRECTORY "${manual_dir}" DESTINATION "CyModelHub.app/Contents/Resources" COMPONENT Hub)
    endif()
    install(DIRECTORY "${manual_dir}" docs DESTINATION . COMPONENT Hub)
    install(DIRECTORY locales DESTINATION . COMPONENT Hub)
    install(DIRECTORY "$<TARGET_FILE_DIR:CyModelHub>/licenses" DESTINATION . COMPONENT Hub)
    install(FILES LICENSE LICENSE-MIT README.md README.fr.md BUILD.md VERSION.md VERSION.fr.md DESTINATION .
        COMPONENT Hub)
endif()

if(CYMODELHUB_BUILD_TESTS)
    include(CTest)
    if(BUILD_TESTING)
        add_executable(modelhub_core_tests tests/core.cpp)
        target_link_libraries(modelhub_core_tests PRIVATE modelhub_core)
        target_include_directories(modelhub_core_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
        cymodelhub_configure(modelhub_core_tests)
        add_test(NAME core COMMAND modelhub_core_tests)
        set_tests_properties(core PROPERTIES TIMEOUT 120)

        add_executable(modelhub_complete_tests tests/complete.cpp)
        target_link_libraries(modelhub_complete_tests PRIVATE modelhub_core)
        target_include_directories(modelhub_complete_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
        cymodelhub_configure(modelhub_complete_tests)
        add_test(NAME complete COMMAND modelhub_complete_tests)
        set_tests_properties(complete PROPERTIES TIMEOUT 180)

        add_executable(modelhub_client_tests tests/client.cpp)
        target_link_libraries(modelhub_client_tests PRIVATE cymodelhub_client modelhub_platform CyMCP::MCP)
        target_include_directories(modelhub_client_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
        cymodelhub_configure(modelhub_client_tests)
        add_dependencies(modelhub_client_tests CyModelHubService CyModelHubMcp)
        add_test(NAME client COMMAND modelhub_client_tests "$<TARGET_FILE:CyModelHubService>"
            "$<TARGET_FILE:CyModelHubMcp>")
        set_tests_properties(client PROPERTIES TIMEOUT 120)

        # The client's HTTP exchange and the installation, each against a local HTTP server;
        # skipped when Python is not available.
        add_executable(modelhub_http_probe tests/http_probe.cpp)
        target_link_libraries(modelhub_http_probe PRIVATE modelhub_platform)
        target_include_directories(modelhub_http_probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
        cymodelhub_configure(modelhub_http_probe)
        find_package(Python3 QUIET COMPONENTS Interpreter)
        if(Python3_Interpreter_FOUND)
            add_test(NAME http COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/local_http.py"
                "$<TARGET_FILE:modelhub_http_probe>")
            set_tests_properties(http PROPERTIES TIMEOUT 120)
            add_test(NAME download COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/download.py"
                "$<TARGET_FILE:CyModelHubCli>" "$<TARGET_FILE:CyModelHubService>")
            set_tests_properties(download PROPERTIES TIMEOUT 180)
            add_test(NAME installs COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/installs.py"
                "$<TARGET_FILE:CyModelHubCli>" "$<TARGET_FILE:CyModelHubService>")
            set_tests_properties(installs PROPERTIES TIMEOUT 300)
            add_test(NAME retry COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/retry.py"
                "$<TARGET_FILE:CyModelHubCli>")
            set_tests_properties(retry PROPERTIES TIMEOUT 120)
        endif()
    endif()
endif()
