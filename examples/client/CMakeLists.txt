# SPDX-FileCopyrightText: 2026 Cyberalien
# SPDX-License-Identifier: MIT

# How an application embeds the CyModelHub client library. Only the client is built: the
# service, the desktop application and the tests stay out of the host project.
cmake_minimum_required(VERSION 3.24)
project(CyModelHubClientExample LANGUAGES CXX)

# In your project: add_subdirectory(external/CyModelHub), or FetchContent with a pinned commit.
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../.." cymodelhub)

# find_model: where is this model? ensure_model: the whole path of an application that needs
# one exact model (strict check, shared path, joining an installation, registering its own).
add_executable(find_model main.cpp)
add_executable(ensure_model ensure_model.cpp)
foreach(example find_model ensure_model)
    target_link_libraries(${example} PRIVATE CyModelHub::Client)
    if(MSVC)
        target_compile_options(${example} PRIVATE /utf-8 /W4 /EHsc)
    endif()
endforeach()
