# Real-ESRGAN ncnn for the HD UI pack (HDUIPack.exe). Built by ..\build.ps1; see ..\README.txt.
#   HDUI_VULKAN=ON  -> realesrgan-ncnn-vulkan.exe (Vulkan GPU, plus -g -1 for the CPU)
#   HDUI_VULKAN=OFF -> realesrgan-ncnn-cpu.exe    (no Vulkan code at all: cannot touch the GPU)
cmake_minimum_required(VERSION 3.16)
cmake_policy(SET CMP0091 NEW)
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

project(hdui_realesrgan C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(HDUI_VULKAN "build the Vulkan GPU version" ON)
set(NCNN_SRC "" CACHE PATH "ncnn source tree (git checkout of the pinned commit, with the glslang submodule)")
set(HDUI_NCNN_VERSION "unknown" CACHE STRING "ncnn version text shown in -h")

if(NOT EXISTS "${NCNN_SRC}/CMakeLists.txt")
    message(FATAL_ERROR "NCNN_SRC must point to an ncnn source tree")
endif()

find_package(OpenMP)

set(NCNN_BUILD_WITH_STATIC_CRT ON CACHE BOOL "" FORCE)
set(NCNN_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(NCNN_INSTALL_SDK OFF CACHE BOOL "" FORCE)
set(NCNN_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(NCNN_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(NCNN_BUILD_BENCHMARK OFF CACHE BOOL "" FORCE)
set(NCNN_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(NCNN_PIXEL_ROTATE OFF CACHE BOOL "" FORCE)
set(NCNN_PIXEL_AFFINE OFF CACHE BOOL "" FORCE)
set(NCNN_PIXEL_DRAWING OFF CACHE BOOL "" FORCE)
set(NCNN_VULKAN ${HDUI_VULKAN} CACHE BOOL "" FORCE)
# ncnn's own Vulkan loader: vulkan-1.dll is loaded only when the GPU is used (never for -g -1)
set(NCNN_SIMPLEVK ON CACHE BOOL "" FORCE)
set(NCNN_VULKAN_ONLINE_SPIRV ON CACHE BOOL "" FORCE)

# Both builds keep only the layers the ESRGAN / Real-ESRGAN models use, and what ncnn creates internally for them
# (padding / crop for the tile borders, packing / cast for layouts). A model with another layer fails to load
# ("layer ... not exists or registered"), it is never run wrongly.
set(HDUI_KEEP_LAYERS input convolution innerproduct relu prelu binaryop eltwise concat split interp pixelshuffle
    padding crop packing cast flatten reshape gemm convolutiondepthwise memorydata noop)
file(STRINGS "${NCNN_SRC}/src/CMakeLists.txt" NCNN_LAYER_LINES REGEX "^ncnn_add_layer\\(")
foreach(LINE ${NCNN_LAYER_LINES})
    string(REGEX REPLACE "^ncnn_add_layer\\(([A-Za-z0-9_]+).*" "\\1" NAME "${LINE}")
    string(TOLOWER "${NAME}" NAME)
    list(FIND HDUI_KEEP_LAYERS "${NAME}" KEEP)
    if(KEEP EQUAL -1)
        set(WITH_LAYER_${NAME} OFF CACHE BOOL "" FORCE)
    else()
        set(WITH_LAYER_${NAME} ON CACHE BOOL "" FORCE)
    endif()
endforeach()

add_subdirectory(${NCNN_SRC} ncnn)

set(HDUI_SOURCES main.cpp realesrgan.cpp)

if(HDUI_VULKAN)
    set(OUT_NAME realesrgan-ncnn-vulkan)

    find_program(GLSLANGVALIDATOR_EXECUTABLE NAMES glslangValidator PATHS $ENV{VULKAN_SDK}/bin $ENV{VULKAN_SDK}/Bin NO_CMAKE_FIND_ROOT_PATH)
    if(NOT GLSLANGVALIDATOR_EXECUTABLE)
        message(FATAL_ERROR "glslangValidator not found (install the Vulkan SDK or set VULKAN_SDK)")
    endif()

    set(SHADER_SPV_HEX_FILES)
    macro(compile_shader SHADER_SRC)
        get_filename_component(NAME_WE ${SHADER_SRC} NAME_WE)
        foreach(VARIANT "" "_fp16s" "_int8s")
            if(VARIANT STREQUAL "_fp16s")
                set(DEFS -DNCNN_fp16_storage=1)
            elseif(VARIANT STREQUAL "_int8s")
                set(DEFS -DNCNN_fp16_storage=1 -DNCNN_int8_storage=1)
            else()
                set(DEFS)
            endif()
            set(HEX ${CMAKE_CURRENT_BINARY_DIR}/${NAME_WE}${VARIANT}.spv.hex.h)
            add_custom_command(OUTPUT ${HEX}
                COMMAND ${GLSLANGVALIDATOR_EXECUTABLE} ${DEFS} -V -s -x -o ${HEX} ${CMAKE_CURRENT_SOURCE_DIR}/${SHADER_SRC}
                DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${SHADER_SRC}
                VERBATIM)
            list(APPEND SHADER_SPV_HEX_FILES ${HEX})
        endforeach()
    endmacro()
    compile_shader(realesrgan_preproc.comp)
    compile_shader(realesrgan_postproc.comp)
    compile_shader(realesrgan_preproc_tta.comp)
    compile_shader(realesrgan_postproc_tta.comp)
    add_custom_target(generate-spirv DEPENDS ${SHADER_SPV_HEX_FILES})
else()
    set(OUT_NAME realesrgan-ncnn-cpu)
endif()

add_executable(${OUT_NAME} ${HDUI_SOURCES})
target_include_directories(${OUT_NAME} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_compile_definitions(${OUT_NAME} PRIVATE HDUI_NCNN_VERSION="${HDUI_NCNN_VERSION}" _CRT_SECURE_NO_WARNINGS NOMINMAX)
if(HDUI_VULKAN)
    add_dependencies(${OUT_NAME} generate-spirv)
endif()
target_link_libraries(${OUT_NAME} PRIVATE ncnn windowscodecs ole32)
if(OpenMP_CXX_FOUND)
    target_link_libraries(${OUT_NAME} PRIVATE OpenMP::OpenMP_CXX)
endif()
