cmake_minimum_required(VERSION 3.25)
project(MultiLanguageHttpClient LANGUAGES C CXX)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(include)

link_directories(BEFORE /usr/local/lib64/)
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -L/usr/local/lib64/ -Wl,-rpath,/usr/local/lib64/")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=native")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -march=native")
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(STATUS "Enabling coverage flags for Debug build")
    add_compile_options(-fprofile-arcs -ftest-coverage)
    add_link_options(-fprofile-arcs -ftest-coverage)
endif()

# Enable LTO globally if supported by the compiler and not in Debug
include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)

include(FetchContent)

FetchContent_Declare(
    Corrosion
    GIT_REPOSITORY https://github.com/corrosion-rs/corrosion.git
    GIT_TAG v0.5.2
)
FetchContent_MakeAvailable(Corrosion)

FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.17.0
)

set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googletest)


set(Python_EXECUTABLE ${CMAKE_SOURCE_DIR}/.venv/bin/python3.12)

find_package(Python 3.12 REQUIRED COMPONENTS Interpreter)

enable_testing()
add_subdirectory(src/c)
add_subdirectory(src/cpp)
add_subdirectory(tests)
add_subdirectory(benchmark)

corrosion_import_crate(
        MANIFEST_PATH src/rust/Cargo.toml
        BINS httprust_client reqwest_client
)

add_custom_target(python_wheel
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_SOURCE_DIR}/src/python/build
    COMMAND ${Python_EXECUTABLE} -m build --wheel
            --outdir ${CMAKE_BINARY_DIR}/wheelhouse
            ${CMAKE_SOURCE_DIR}/src/python
    COMMENT "Building Python wheel..."
    VERBATIM
)

add_dependencies(httpc_lib python_wheel)
add_dependencies(httpcpp_lib python_wheel)


install(TARGETS httpc_lib httpcpp_lib
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib
        RUNTIME DESTINATION bin
)

install(DIRECTORY include/ DESTINATION include)

set(CPACK_PACKAGE_NAME "http-client-cpp-sdk")
set(CPACK_PACKAGE_VENDOR "Warren Jitsing")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Multi-Language HTTP Client SDK (C/C++)")
set(CPACK_PACKAGE_VERSION "1.0.0")
set(CPACK_PACKAGE_CONTACT "warren.jitsing@infcon.co.za")

set(CPACK_GENERATOR "TGZ")

include(CPack)