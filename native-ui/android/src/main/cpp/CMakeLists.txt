cmake_minimum_required(VERSION 3.22)
project(emucorer_ps1ui CXX)

add_library(emucorer_ps1ui SHARED
    ps1_jni.cpp
    ps1_renderer.cpp
)

target_compile_features(emucorer_ps1ui PRIVATE cxx_std_17)
target_compile_options(emucorer_ps1ui PRIVATE -Wall -Wextra -Wno-unused-parameter)

find_library(android-lib android)
find_library(log-lib log)
find_library(GLESv3-lib GLESv3)
target_link_libraries(emucorer_ps1ui ${android-lib} ${log-lib} ${GLESv3-lib})
