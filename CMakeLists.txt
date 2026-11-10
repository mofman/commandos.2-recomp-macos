cmake_minimum_required(VERSION 3.24)
project(Commandos2Native C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_OSX_ARCHITECTURES arm64)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
find_package(SDL3 REQUIRED CONFIG PATHS /opt/homebrew/opt/sdl3)

file(GLOB GEN_SRC CONFIGURE_DEPENDS generated/gen_*.c)
add_library(c2gen STATIC ${GEN_SRC} generated/dispatch_table.c generated/import_table.c generated/patches.c)
target_include_directories(c2gen PUBLIC runtime generated)
# generated code: warnings off, no strict-overflow assumptions (x86 wraps)
target_compile_options(c2gen PRIVATE -w -fwrapv -fno-strict-aliasing -O2)

file(GLOB RT_SRC CONFIGURE_DEPENDS runtime/*.c runtime/win32/*.c runtime/dx/*.c runtime/overrides/*.c)
add_executable(Commandos2 ${RT_SRC} generated/import_stubs.c)
target_compile_options(Commandos2 PRIVATE -Wall -Wno-unused-function -fwrapv -fno-strict-aliasing)
target_link_libraries(Commandos2 PRIVATE c2gen SDL3::SDL3 "-framework CoreFoundation")

# Optional: self-contained "dist/Commandos 2.app" (game data, embedded SDL3, icon). Not part of the default build.
add_custom_target(package_app
  COMMAND ${CMAKE_SOURCE_DIR}/packaging/package_app.sh $<TARGET_FILE:Commandos2>
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  DEPENDS Commandos2
  USES_TERMINAL)
