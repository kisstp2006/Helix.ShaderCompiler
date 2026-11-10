cmake_minimum_required(VERSION 3.28)
project(Helix.ShaderCompiler LANGUAGES CXX)

if(NOT COMMAND helix_add_library)
    include("${CMAKE_CURRENT_SOURCE_DIR}/External/Helix.CMake/Helix.cmake")
endif()

helix_require(Core)
helix_add_library(helix_shader_compiler ALIAS ShaderCompiler PUBLIC_DEPS Helix::Core)

helix_add_test_suite(helix_shader_compiler_tests DEPS Helix::ShaderCompiler)

# The tests read real assemblies: the C# test shaders are built with the .NET SDK and embedded as byte arrays.
if(TARGET helix_shader_compiler_tests)
    find_program(HELIX_DOTNET NAMES dotnet HINTS "$ENV{HELIX_DOTNET_ROOT}" "$ENV{DOTNET_ROOT}" REQUIRED)
    set(artifacts "${CMAKE_CURRENT_BINARY_DIR}/Dotnet")
    set(generated "${CMAKE_CURRENT_BINARY_DIR}/Generated")
    set(tests_dll "${artifacts}/bin/Helix.ShaderTests/release/Helix.ShaderTests.dll")
    set(library_dll "${artifacts}/bin/Helix.ShaderTests/release/HelixEngine.Shaders.dll")
    file(GLOB_RECURSE shader_sources CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/Tests/Shaders/*.cs" "${CMAKE_CURRENT_SOURCE_DIR}/Tests/Shaders/*.csproj"
        "${CMAKE_CURRENT_SOURCE_DIR}/External/Helix.Shaders/Source/*.cs")
    list(FILTER shader_sources EXCLUDE REGEX "/(bin|obj)/")
    add_custom_command(
        OUTPUT "${tests_dll}" "${library_dll}"
        COMMAND "${HELIX_DOTNET}" build "${CMAKE_CURRENT_SOURCE_DIR}/Tests/Shaders/Helix.ShaderTests.csproj" -c Release
                --artifacts-path "${artifacts}" --nologo -v quiet
        DEPENDS ${shader_sources}
        COMMENT "Building the C# test shaders")
    add_custom_command(
        OUTPUT "${generated}/TestShadersAssembly.h" "${generated}/ShaderLibraryAssembly.h"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${generated}"
        COMMAND "${CMAKE_COMMAND}" "-DINPUT=${tests_dll}" "-DOUTPUT=${generated}/TestShadersAssembly.h" -DNAME=kTestShadersAssembly
                -P "${CMAKE_CURRENT_SOURCE_DIR}/Tools/EmbedFile.cmake"
        COMMAND "${CMAKE_COMMAND}" "-DINPUT=${library_dll}" "-DOUTPUT=${generated}/ShaderLibraryAssembly.h" -DNAME=kShaderLibraryAssembly
                -P "${CMAKE_CURRENT_SOURCE_DIR}/Tools/EmbedFile.cmake"
        DEPENDS "${tests_dll}" "${library_dll}" "${CMAKE_CURRENT_SOURCE_DIR}/Tools/EmbedFile.cmake"
        COMMENT "Embedding the test assemblies")
    target_sources(helix_shader_compiler_tests PRIVATE "${generated}/TestShadersAssembly.h" "${generated}/ShaderLibraryAssembly.h")
    target_include_directories(helix_shader_compiler_tests PRIVATE "${generated}")
endif()
