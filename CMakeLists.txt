cmake_minimum_required(VERSION 3.10)

project(ImageEditing)
set(SRC_DIR ${PROJECT_SOURCE_DIR}/src/)
set(INCLUDE_DIR ${PROJECT_SOURCE_DIR}/include/)
set(LIB_DIR ${PROJECT_SOURCE_DIR}/lib/)

include_directories(${INCLUDE_DIR})
include_directories(${LIB_DIR})

# add C/C++ > preprocess: "XKEYCHECK_H"
add_Definitions("-D_XKEYCHECK_H")
add_definitions(-DPROJECT_DIR="${PROJECT_SOURCE_DIR}")

add_executable(ImageEditing 
    ${SRC_DIR}Main.cpp
    ${SRC_DIR}Globals.h
    ${SRC_DIR}Globals.inl
    ${SRC_DIR}ImageWidget.h
    ${SRC_DIR}ImageWidget.cpp
    ${SRC_DIR}ScriptHandler.h
    ${SRC_DIR}ScriptHandler.cpp
    ${SRC_DIR}TargaImage.h
    ${SRC_DIR}TargaImage.cpp)

add_library(libtarga ${SRC_DIR}libtarga.h ${SRC_DIR}libtarga.c)

target_link_libraries(ImageEditing 
debug ${LIB_DIR}Debug/fltk_formsd.lib      optimized ${LIB_DIR}Release/fltk_forms.lib
debug ${LIB_DIR}Debug/fltk_gld.lib         optimized ${LIB_DIR}Release/fltk_gl.lib
debug ${LIB_DIR}Debug/fltk_imagesd.lib     optimized ${LIB_DIR}Release/fltk_images.lib
debug ${LIB_DIR}Debug/fltk_jpegd.lib       optimized ${LIB_DIR}Release/fltk_jpeg.lib
debug ${LIB_DIR}Debug/fltk_pngd.lib        optimized ${LIB_DIR}Release/fltk_png.lib
debug ${LIB_DIR}Debug/fltk_zd.lib          optimized ${LIB_DIR}Release/fltk_z.lib
debug ${LIB_DIR}Debug/fltkd.lib            optimized ${LIB_DIR}Release/fltk.lib)

target_link_libraries(ImageEditing libtarga)

option(IMAGEEDITING_BUILD_PORTABLE "Enable the Windows x64 portable ZIP package" OFF)
if(IMAGEEDITING_BUILD_PORTABLE)
    if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "The supplied FLTK libraries require an MSVC x64 build.")
    endif()

    target_compile_definitions(ImageEditing PRIVATE IMAGEEDITING_PORTABLE)

    # Copy the compiler's redistributable release CRT beside the executable.
    # Windows 10/11 already provide the Universal CRT; do not copy system DLLs.
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
    set(CMAKE_INSTALL_DEBUG_LIBRARIES FALSE)
    set(CMAKE_INSTALL_DEBUG_LIBRARIES_ONLY FALSE)
    set(CMAKE_INSTALL_UCRT_LIBRARIES FALSE)
    include(InstallRequiredSystemLibraries)
    if(NOT CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
        message(FATAL_ERROR "MSVC redistributable DLLs were not found. Install the Visual Studio C++ tools.")
    endif()
    foreach(runtime IN LISTS CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
        if(NOT EXISTS "${runtime}")
            message(FATAL_ERROR "Missing MSVC redistributable DLL: ${runtime}")
        endif()
    endforeach()

    install(CODE [[
        if(NOT CMAKE_INSTALL_CONFIG_NAME STREQUAL "Release")
            message(FATAL_ERROR "Build the portable package with --config Release.")
        endif()
    ]])
    install(TARGETS ImageEditing RUNTIME DESTINATION . CONFIGURATIONS Release)
    install(PROGRAMS ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS}
        DESTINATION . CONFIGURATIONS Release)
    install(DIRECTORY "${PROJECT_SOURCE_DIR}/Images" DESTINATION .)
    install(DIRECTORY DESTINATION Output)
    install(FILES portable/Start.bat portable/demo.txt portable/README.txt DESTINATION .)
    install(DIRECTORY portable/Demo portable/Reference DESTINATION .)
    install(FILES include/stb/LICENSE DESTINATION Licenses RENAME stb.txt)

    set(CPACK_GENERATOR ZIP)
    set(CPACK_PACKAGE_NAME P1-Demo)
    set(CPACK_PACKAGE_VERSION 1.0.0)
    set(CPACK_PACKAGE_FILE_NAME P1-Demo-Windows-x64)
    set(CPACK_PACKAGE_DIRECTORY "${PROJECT_SOURCE_DIR}/dist")
    set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
    set(CPACK_VERBATIM_VARIABLES YES)
    include(CPack)
endif()
