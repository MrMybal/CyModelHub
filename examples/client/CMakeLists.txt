# SPDX-FileCopyrightText: 2026 Cyberalien
# SPDX-License-Identifier: MIT

# How an application embeds the CyModelHub client library. Only the client is built: the
# service, the desktop application and the tests stay out of the host project.
cmake_minimum_required(VERSION 3.24)
project(CyModelHubClientExample LANGUAGES CXX)

# In your project: add_subdirectory(external/CyModelHub), or FetchContent with a pinned commit.
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../.." cymodelhub)

add_executable(find_model main.cpp)
target_link_libraries(find_model PRIVATE CyModelHub::Client)
if(MSVC)
    target_compile_options(find_model PRIVATE /utf-8 /W4 /EHsc)
endif()
