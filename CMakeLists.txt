cmake_minimum_required(VERSION 3.16)
project(nextui_app_sdk VERSION 1.0.0 LANGUAGES C)

option(NEXTUI_APP_BUILD_TESTS "Build host tests" ON)
option(NEXTUI_APP_BUILD_EXAMPLE "Build the overlay demo" ON)

find_package(Threads REQUIRED)

add_library(nextui_app STATIC
    src/nextui_app.c
    src/nextui_json.c
    src/nextui_transport.c
)
add_library(nextui_app::nextui_app ALIAS nextui_app)

target_include_directories(nextui_app
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(nextui_app PUBLIC c_std_99)
target_compile_definitions(nextui_app PRIVATE _POSIX_C_SOURCE=200809L)
target_link_libraries(nextui_app PUBLIC Threads::Threads)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(nextui_app PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(NEXTUI_APP_BUILD_EXAMPLE)
    add_executable(nextui-overlay-demo examples/overlay_demo.c)
    target_link_libraries(nextui-overlay-demo PRIVATE nextui_app)
endif()

if(NEXTUI_APP_BUILD_TESTS)
    enable_testing()

    add_executable(fake_devserv tests/fake_devserv.c)
    target_compile_definitions(fake_devserv PRIVATE _POSIX_C_SOURCE=200809L)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(fake_devserv PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    add_executable(test_json tests/test_json.c)
    target_link_libraries(test_json PRIVATE nextui_app)

    add_executable(test_client tests/test_client.c)
    target_link_libraries(test_client PRIVATE nextui_app)

    add_test(NAME json COMMAND test_json)
    add_test(NAME client COMMAND test_client $<TARGET_FILE:fake_devserv>)
endif()

install(TARGETS nextui_app ARCHIVE DESTINATION lib)
install(FILES include/nextui_app.h DESTINATION include)
