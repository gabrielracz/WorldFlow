cmake_minimum_required (VERSION 3.8)
set(PROJ_NAME "engine")
project (PROJ_NAME)
# set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/bin")
# set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/bin")
set(CMAKE_CXX_STANDARD 20)
set(LIBRARY_PATH ${CMAKE_CURRENT_SOURCE_DIR}/libs)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_BUILD_TYPE DEBUG)
add_compile_options("/DEBUG")

#sources
file(GLOB_RECURSE SRCS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
file(GLOB_RECURSE HDRS "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp")

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/include/PathConfig.hpp.in ${CMAKE_CURRENT_SOURCE_DIR}/include/PathConfig.hpp)
add_executable(${PROJ_NAME} WIN32 ${HDRS} ${SRCS})
# set_target_properties(${PROJ_NAME} PROPERTIES DEBUG_POSTFIX _d)

# add_definitions(${COMPILER_FLAGS})
target_include_directories(${PROJ_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(${PROJ_NAME} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/libs/vulkan")
target_include_directories(${PROJ_NAME} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/libs/glfw")
target_include_directories(${PROJ_NAME} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/libs/glfw")
# target_include_directories(${PROJ_NAME} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/libs/fmt/include")

# libraries
find_package(Vulkan REQUIRED)
add_subdirectory(libs)
add_subdirectory(shared)

target_include_directories(${PROJ_NAME} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(${PROJ_NAME} vkguide_shared)
target_link_libraries(${PROJ_NAME} Vulkan::Vulkan sdl2)
target_link_libraries(${PROJ_NAME} vkbootstrap vma glm imgui stb_image fastgltf::fastgltf)
# target_link_libraries(${PROJ_NAME} Vulkan::Vulkan)
if(NOT WIN32)
    find_library(GLFW_LIBRARY glfw)
elseif(WIN32)
    find_library(GLFW_LIBRARY glfw3 HINTS ${LIBRARY_PATH}/glfw)
endif(NOT WIN32)

target_link_libraries(${PROJ_NAME} ${GLFW_LIBRARY})

# shaders
find_program(GLSL_VALIDATOR glslangValidator HINTS /usr/bin /usr/local/bin $ENV{VULKAN_SDK}/Bin/ $ENV{VULKAN_SDK}/Bin32/)
file(GLOB_RECURSE GLSL_SOURCE_FILES
    "${PROJECT_SOURCE_DIR}/shaders/*.frag"
    "${PROJECT_SOURCE_DIR}/shaders/*.vert"
    "${PROJECT_SOURCE_DIR}/shaders/*.comp"
)

foreach(GLSL ${GLSL_SOURCE_FILES})
  message(STATUS "BUILDING SHADER")
  get_filename_component(FILE_NAME ${GLSL} NAME)
  set(SPIRV "${PROJECT_SOURCE_DIR}/shaders/${FILE_NAME}.spv")
  message(STATUS ${GLSL})
  message(STATUS COMMAND ${GLSL_VALIDATOR} -V ${GLSL} -o ${SPIRV})
  add_custom_command(
    OUTPUT ${SPIRV}
    COMMAND ${GLSL_VALIDATOR} -V ${GLSL} -o ${SPIRV}
    DEPENDS ${GLSL})
  list(APPEND SPIRV_BINARY_FILES ${SPIRV})
endforeach(GLSL)

add_custom_target(
    Shaders 
    DEPENDS ${SPIRV_BINARY_FILES}
    )

#     if(WIN32)
#     # Avoid ZERO_CHECK target in Visual Studio
#     set(CMAKE_SUPPRESS_REGENERATION TRUE)

#     set_target_properties(${PROJ_NAME} PROPERTIES
#         VS_GLOBAL_CXX_FLAGS "${VS_GLOBAL_CXX_FLAGS} /STACK:8388608"
#     )
    
#     target_compile_options(${PROJ_NAME} PRIVATE "/bigobj")
 
#     # This will use the proper libraries in debug mode in Visual Studio
#     set_target_properties(${PROJ_NAME} PROPERTIES DEBUG_POSTFIX _d)

#     # Specify the source directory containing DLLs
#     set(SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/libs/lib)

#     # Specify the destination directory
#     set(DESTINATION_DIR ${CMAKE_CURRENT_BINARY_DIR}/Release)

#     # Get a list of all DLLs in the source directory
#     file(GLOB DLLS ${SOURCE_DIR}/*.dll)

#     # Create a custom command for each DLL
#     foreach(DLL ${DLLS})
#         get_filename_component(DLL_NAME ${DLL} NAME)
#         add_custom_command(
#             TARGET ${PROJ_NAME} POST_BUILD
#             COMMAND ${CMAKE_COMMAND} -E copy_if_different
#                 ${DLL}
#                 ${DESTINATION_DIR}/${DLL_NAME}
#             COMMENT "Copying ${DLL_NAME} to ${DESTINATION_DIR}"
#         )
#     endforeach()

#     # Create a custom target to group the commands
#     add_custom_target(CopyDLLs ALL DEPENDS ${DLLS})
# endif(WIN32)