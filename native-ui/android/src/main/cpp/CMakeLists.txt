cmake_minimum_required(VERSION 3.22)
project(emucorer_ps1ui CXX)

# Google Play 16 KiB page alignment support.
add_link_options("-Wl,-z,max-page-size=16384" "-Wl,-z,common-page-size=16384")

option(PS1UI_ENABLE_LTO "Enable ThinLTO for the native PS1 UI renderer" ON)

add_library(emucorer_ps1ui SHARED
    ps1_jni.cpp
    ps1_renderer.cpp
)

target_compile_features(emucorer_ps1ui PRIVATE cxx_std_17)
target_compile_options(emucorer_ps1ui PRIVATE
    -Wall -Wextra -Wno-unused-parameter
    -O3
    -fno-exceptions -fno-rtti
)

if(PS1UI_ENABLE_LTO AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(emucorer_ps1ui PRIVATE "-flto=thin")
    target_link_options(emucorer_ps1ui PRIVATE "-flto=thin")
endif()

set_target_properties(emucorer_ps1ui PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

find_library(android-lib android)
find_library(log-lib log)
find_library(GLESv3-lib GLESv3)
target_link_libraries(emucorer_ps1ui PRIVATE ${android-lib} ${log-lib} ${GLESv3-lib})
