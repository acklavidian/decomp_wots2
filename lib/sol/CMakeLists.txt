# Sol: Way of the Samurai 2's event-script language — the VM and the compiler.
# Standalone (C++17, no dependencies): add_subdirectory(sol) and link `sol`.
cmake_minimum_required(VERSION 3.16)
project(sol CXX)

add_library(sol STATIC src/vm.cpp src/compiler.cpp)
target_include_directories(sol PUBLIC include)
target_compile_features(sol PUBLIC cxx_std_17)

add_executable(solc tools/solc.cpp)
target_link_libraries(solc PRIVATE sol)
add_executable(solrun tools/solrun.cpp)
target_link_libraries(solrun PRIVATE sol)
if(MINGW)
    # Self-contained executables: no MinGW runtime DLLs needed beside them.
    target_link_options(solc PRIVATE -static)
    target_link_options(solrun PRIVATE -static)
endif()
