# PLAN C6: fingerprint the implementation inputs, not a checkout or timestamp.
# Call after runtime targets have their compile/link settings. Nothing is read
# at game startup: the digest becomes a constant in the handshake translation unit.
function(stv_runtime_fingerprint target)
  set(inputs "${CMAKE_SOURCE_DIR}/CMakeLists.txt" "${CMAKE_CURRENT_FUNCTION_LIST_FILE}"
    "${CMAKE_SOURCE_DIR}/data/enhanced_patchpoints.json"
    "${CMAKE_SOURCE_DIR}/data/enhanced_data_roots.json"
    "${CMAKE_SOURCE_DIR}/data/enhanced_scene_writes.json"
    "${CMAKE_SOURCE_DIR}/data/enhanced_balance.json"
    "${CMAKE_SOURCE_DIR}/data/enhanced_stage_balance.json")
  set(settings "stv-runtime-identity-v1\n")
  foreach(runtime_target IN ITEMS stv_base stv_gsp stv_l8 stv_snd stv_game stv_net stv_pc_frontend SmashTV)
    if(NOT TARGET ${runtime_target})
      continue()
    endif()
    get_target_property(sources ${runtime_target} SOURCES)
    foreach(source IN LISTS sources)
      if(source MATCHES "\\$<")
        message(FATAL_ERROR "Runtime fingerprint cannot inventory generated source expression: ${source}")
      endif()
      if(NOT IS_ABSOLUTE "${source}")
        set(source "${CMAKE_SOURCE_DIR}/${source}")
      endif()
      list(APPEND inputs "${source}")
      foreach(property IN ITEMS COMPILE_OPTIONS COMPILE_DEFINITIONS COMPILE_FLAGS)
        get_source_file_property(value "${source}" ${property})
        if(value AND NOT value STREQUAL "NOTFOUND")
          file(RELATIVE_PATH name "${CMAKE_SOURCE_DIR}" "${source}")
          string(APPEND settings "source:${name}:${property}=${value}\n")
        endif()
      endforeach()
    endforeach()
    foreach(property IN ITEMS COMPILE_OPTIONS COMPILE_DEFINITIONS COMPILE_FEATURES
        INCLUDE_DIRECTORIES INTERFACE_COMPILE_OPTIONS INTERFACE_COMPILE_DEFINITIONS
        INTERFACE_COMPILE_FEATURES COMPILE_FLAGS LINK_FLAGS LINK_OPTIONS
        INTERFACE_LINK_OPTIONS LINK_LIBRARIES INTERFACE_LINK_LIBRARIES
        MSVC_RUNTIME_LIBRARY CXX_STANDARD CXX_EXTENSIONS)
      get_target_property(value ${runtime_target} ${property})
      if(value AND NOT value MATCHES "-NOTFOUND$")
        string(APPEND settings "target:${runtime_target}:${property}=${value}\n")
      endif()
    endforeach()
  endforeach()
  # These directories contain the headers of the compiled runtime, including
  # inline opcode/platform operations and the vendored YM implementation.
  file(GLOB_RECURSE headers CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/include/stv/gsp/*.hpp"
    "${CMAKE_SOURCE_DIR}/include/stv/snd/*.hpp"
    "${CMAKE_SOURCE_DIR}/include/stv/game/*.hpp"
    "${CMAKE_SOURCE_DIR}/include/stv/host/*.hpp"
    "${CMAKE_SOURCE_DIR}/include/stv/net/*.hpp"
    "${CMAKE_SOURCE_DIR}/src/host/*.hpp"
    "${CMAKE_SOURCE_DIR}/src/host/*.hlsl"
    "${CMAKE_SOURCE_DIR}/third_party/ymfm/*.h"
    "${CMAKE_SOURCE_DIR}/third_party/ymfm/*.ipp")
  list(APPEND inputs ${headers} "${CMAKE_SOURCE_DIR}/include/stv/base.hpp"
    "${CMAKE_SOURCE_DIR}/include/stv/zip.hpp"
    "${CMAKE_SOURCE_DIR}/include/stv/reference_timing.hpp")
  list(REMOVE_DUPLICATES inputs)
  list(SORT inputs)
  set(manifest "")
  foreach(input IN LISTS inputs)
    if(NOT EXISTS "${input}")
      message(FATAL_ERROR "Missing runtime fingerprint input: ${input}")
    endif()
    file(RELATIVE_PATH name "${CMAKE_SOURCE_DIR}" "${input}")
    if(name MATCHES "^\\.\\.")
      message(FATAL_ERROR "Runtime fingerprint input is outside the source tree: ${input}")
    endif()
    file(SHA256 "${input}" digest)
    string(APPEND manifest "${name} ${digest}\n")
  endforeach()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${inputs})
  foreach(variable IN ITEMS CMAKE_CXX_COMPILER_ID CMAKE_CXX_COMPILER_VERSION
      CMAKE_CXX_COMPILER_TARGET CMAKE_CXX_COMPILER_ARCHITECTURE_ID CMAKE_CXX_BYTE_ORDER
      CMAKE_SYSTEM_NAME CMAKE_SYSTEM_PROCESSOR
      CMAKE_SIZEOF_VOID_P CMAKE_CXX_STANDARD CMAKE_CXX_EXTENSIONS
      CMAKE_MSVC_RUNTIME_LIBRARY STV_SANITIZE CMAKE_CXX_FLAGS CMAKE_EXE_LINKER_FLAGS)
    string(APPEND settings "${variable}=${${variable}}\n")
  endforeach()
  foreach(variable IN ITEMS CL _CL_ CXXFLAGS CPPFLAGS LDFLAGS VCToolsVersion WindowsSDKVersion)
    string(APPEND settings "environment:${variable}=$ENV{${variable}}\n")
  endforeach()
  # The same source/configuration must identify equally at a different checkout
  # path. Keep caller flags themselves; normalize only these two known roots.
  string(REPLACE "${CMAKE_SOURCE_DIR}" "<source>" settings "${settings}")
  string(REPLACE "${CMAKE_BINARY_DIR}" "<build>" settings "${settings}")
  if(CMAKE_CONFIGURATION_TYPES)
    set(configurations ${CMAKE_CONFIGURATION_TYPES})
  elseif(CMAKE_BUILD_TYPE)
    set(configurations "${CMAKE_BUILD_TYPE}")
  else()
    message(FATAL_ERROR "Runtime fingerprint requires a named build configuration")
  endif()
  foreach(configuration IN LISTS configurations)
    string(TOUPPER "${configuration}" upper_configuration)
    set(configuration_settings "configuration=${configuration}\nCMAKE_CXX_FLAGS_CONFIG=${CMAKE_CXX_FLAGS_${upper_configuration}}\nCMAKE_EXE_LINKER_FLAGS_CONFIG=${CMAKE_EXE_LINKER_FLAGS_${upper_configuration}}\n")
    string(SHA256 digest "${manifest}${settings}${configuration_settings}")
    set(bytes "")
    foreach(offset RANGE 0 62 2)
      string(SUBSTRING "${digest}" ${offset} 2 byte)
      string(APPEND bytes "0x${byte},")
    endforeach()
    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/runtime-identity/${configuration}/stv/net/build_identity.hpp"
      CONTENT "#pragma once\n#include <array>\n#include <cstdint>\nnamespace stv::net { inline constexpr std::array<std::uint8_t, 32> kBuildIdentity = {${bytes}}; }\n"
      CONDITION "$<CONFIG:${configuration}>" NEWLINE_STYLE LF)
    # Preserve the exact hashed text. file(GENERATE) would evaluate generator
    # expressions in target settings and make the receipt differ from its hash.
    file(CONFIGURE OUTPUT "${CMAKE_BINARY_DIR}/runtime-identity/${configuration}/inputs.txt"
      CONTENT "sha256=${digest}\n${manifest}${settings}${configuration_settings}"
      @ONLY NEWLINE_STYLE LF)
  endforeach()
  set(identity_include "${CMAKE_BINARY_DIR}/runtime-identity/$<CONFIG>")
  target_include_directories(${target} PRIVATE "${identity_include}")
  set(STV_RUNTIME_FINGERPRINT_INCLUDE "${identity_include}" PARENT_SCOPE)
endfunction()
