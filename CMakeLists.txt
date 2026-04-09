cmake_minimum_required(VERSION 3.20)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
cmake_policy(SET CMP0167 NEW)

project(
    vdm_rs
    VERSION 0.1
    LANGUAGES C CXX
)

# This linker flag is necessary due to a brew LLVM bug
# See:
# https://forum.juce.com/t/cant-compile-vst3-with-llvm-18-on-arm-mac/62676
# https://geant4-forum.web.cern.ch/t/clang-error-linker-command-failed-with-exit-code-1-use-v-to-see-invocation/12372/2
# https://github.com/Homebrew/homebrew-core/issues/169820
# https://github.com/llvm/llvm-project/issues/77653
# https://github.com/Homebrew/homebrew-core/issues/178435
#set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -L/usr/local/opt/llvm/lib/c++")


set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++")
#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -lc++abi")

add_subdirectory(thirdparty/argparse)

set(DOCTEST_WITH_TESTS OFF)
set(DOCTEST_NO_INSTALL ON)
set(DOCTEST_WITH_MAIN_IN_STATIC_LIB OFF)
add_subdirectory(thirdparty/doctest)

add_library(vdm_rs_project_options INTERFACE)

target_include_directories(vdm_rs_project_options
    INTERFACE
    include
)

add_library(vdm_rs INTERFACE)

target_link_libraries(vdm_rs
    INTERFACE
    vdm_rs_project_options
)

target_compile_options(vdm_rs_project_options INTERFACE
    -Wall -Wextra -Wpedantic
    -Wshadow
    -Wconversion -Wsign-conversion
    -Wformat=2
    -Wundef
    -Wnon-virtual-dtor
    -Woverloaded-virtual
    -Wdouble-promotion
    -Wimplicit-fallthrough
    -Wextra-semi
    -Wcast-qual -Wcast-align
    -Wredundant-decls
    $<$<CONFIG:Debug>:
        -fsanitize=address
        -g
    >
    $<$<CONFIG:Release>:
        -O3
        -DNDEBUG
    >
)

target_link_options(vdm_rs_project_options INTERFACE
    $<$<CONFIG:Debug>:
        -fsanitize=address
    >
)

add_executable(main
    src/main.cpp
)

target_link_libraries(main
    PRIVATE
    argparse
    vdm_rs
)

enable_testing()

file(GLOB TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp"
)

add_executable(unit_tests
    ${TEST_SOURCES}
)

target_link_libraries(unit_tests
    PRIVATE
    doctest::doctest
    vdm_rs
)

add_test(
    NAME unit_tests
    COMMAND unit_tests
)
