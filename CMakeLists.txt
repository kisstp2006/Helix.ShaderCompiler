cmake_minimum_required(VERSION 3.28)
project(Helix.ShaderCompiler LANGUAGES CXX)

if(NOT COMMAND helix_add_library)
    include("${CMAKE_CURRENT_SOURCE_DIR}/External/Helix.CMake/Helix.cmake")
endif()

helix_require(Core)
if(PROJECT_IS_TOP_LEVEL)
    # Only the tests use these: compiled shaders run on the GPU through Helix.RHI.
    helix_require(Platform)
    helix_require(Profiler)
    helix_require(GL)
    helix_require(D3D11)
    helix_require(RHI)
endif()
helix_add_library(helix_shader_compiler ALIAS ShaderCompiler PUBLIC_DEPS Helix::Core)

helix_add_test_suite(helix_shader_compiler_tests DEPS Helix::ShaderCompiler Helix::RHI EXCLUDE "/Support/DiscreteGpu.cpp$")
if(TARGET helix_shader_compiler_tests AND HELIX_PLATFORM STREQUAL "windows")
    # The same tests on the discrete GPU of hybrid graphics machines.
    get_target_property(shader_test_sources helix_shader_compiler_tests SOURCES)
    helix_add_executable(helix_shader_compiler_tests_discrete_gpu SOURCES ${shader_test_sources} Tests/Support/DiscreteGpu.cpp
                         DEPS Helix::ShaderCompiler Helix::RHI Helix::TestMain)
    target_include_directories(helix_shader_compiler_tests_discrete_gpu PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source ${CMAKE_CURRENT_SOURCE_DIR}/Tests)
    add_test(NAME helix_shader_compiler_tests_discrete_gpu COMMAND helix_shader_compiler_tests_discrete_gpu)
endif()

# The tests read real assemblies: the C# test shaders are built with the .NET SDK and embedded as byte arrays.
if(TARGET helix_shader_compiler_tests)
    find_program(HELIX_DOTNET NAMES dotnet HINTS "$ENV{HELIX_DOTNET_ROOT}" "$ENV{DOTNET_ROOT}" REQUIRED)
    set(artifacts "${CMAKE_CURRENT_BINARY_DIR}/Dotnet")
    set(generated "${CMAKE_CURRENT_BINARY_DIR}/Generated")
    set(tests_dll "${artifacts}/bin/Helix.ShaderTests/release/Helix.ShaderTests.dll")
    set(library_dll "${artifacts}/bin/Helix.ShaderTests/release/HelixEngine.Shaders.dll")
    set(oracle_dll "${artifacts}/bin/Helix.ShaderOracle/release/Helix.ShaderOracle.dll")
    set(expectations "${artifacts}/ShaderExpectations.bin")
    file(GLOB_RECURSE shader_sources CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/Tests/Shaders/*.cs" "${CMAKE_CURRENT_SOURCE_DIR}/Tests/Shaders/*.csproj"
        "${CMAKE_CURRENT_SOURCE_DIR}/Tests/Oracle/*.cs" "${CMAKE_CURRENT_SOURCE_DIR}/Tests/Oracle/*.csproj"
        "${CMAKE_CURRENT_SOURCE_DIR}/External/Helix.Shaders/Source/*.cs")
    list(FILTER shader_sources EXCLUDE REGEX "/(bin|obj)/")
    add_custom_command(
        OUTPUT "${tests_dll}" "${library_dll}" "${oracle_dll}"
        COMMAND "${HELIX_DOTNET}" build "${CMAKE_CURRENT_SOURCE_DIR}/Tests/Oracle/Helix.ShaderOracle.csproj" -c Release
                --artifacts-path "${artifacts}" --nologo -v quiet
        DEPENDS ${shader_sources}
        COMMENT "Building the C# test shaders and the oracle")
    # The oracle runs the shaders on the CPU; its output is what the GPU has to reproduce.
    add_custom_command(
        OUTPUT "${expectations}"
        COMMAND "${HELIX_DOTNET}" "${oracle_dll}" "${expectations}"
        DEPENDS "${oracle_dll}" "${tests_dll}"
        COMMENT "Running the test shaders on the CPU")
    set(embedded "${generated}/TestShadersAssembly.h" "${generated}/ShaderLibraryAssembly.h" "${generated}/ShaderExpectations.h")
    add_custom_command(
        OUTPUT ${embedded}
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${generated}"
        COMMAND "${CMAKE_COMMAND}" "-DINPUT=${tests_dll}" "-DOUTPUT=${generated}/TestShadersAssembly.h" -DNAME=kTestShadersAssembly
                -P "${CMAKE_CURRENT_SOURCE_DIR}/Tools/EmbedFile.cmake"
        COMMAND "${CMAKE_COMMAND}" "-DINPUT=${library_dll}" "-DOUTPUT=${generated}/ShaderLibraryAssembly.h" -DNAME=kShaderLibraryAssembly
                -P "${CMAKE_CURRENT_SOURCE_DIR}/Tools/EmbedFile.cmake"
        COMMAND "${CMAKE_COMMAND}" "-DINPUT=${expectations}" "-DOUTPUT=${generated}/ShaderExpectations.h" -DNAME=kShaderExpectations
                -P "${CMAKE_CURRENT_SOURCE_DIR}/Tools/EmbedFile.cmake"
        DEPENDS "${tests_dll}" "${library_dll}" "${expectations}" "${CMAKE_CURRENT_SOURCE_DIR}/Tools/EmbedFile.cmake"
        COMMENT "Embedding the test assemblies and expectations")
    foreach(suite helix_shader_compiler_tests helix_shader_compiler_tests_discrete_gpu)
        if(TARGET ${suite})
            target_sources(${suite} PRIVATE ${embedded})
            target_include_directories(${suite} PRIVATE "${generated}")
        endif()
    endforeach()
endif()
