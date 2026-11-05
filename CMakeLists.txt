cmake_minimum_required(VERSION 3.24)
# Keep configure-time dependency detection and every later MSVC invocation in
# the same language, including builds started outside Build.ps1.
set(ENV{VSLANG} 1033)
project(DLSSNR_LiteDX12 VERSION 1.0 LANGUAGES C CXX ASM_MASM)
if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Windows x64 with MSVC required")
endif()
set(CMAKE_CL_SHOWINCLUDES_PREFIX "Note: including file: ")
set(CMAKE_C_COMPILER_LAUNCHER "${CMAKE_COMMAND};-E;env;VSLANG=1033")
set(CMAKE_CXX_COMPILER_LAUNCHER "${CMAKE_COMMAND};-E;env;VSLANG=1033")
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# Microsoft Detours 4.0.1, vendored under third_party; shipped as the reference uses it.
add_library(detours STATIC IMPORTED)
set_target_properties(detours PROPERTIES IMPORTED_LOCATION "${CMAKE_CURRENT_SOURCE_DIR}/third_party/detours/detours.lib" INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
# Official Dear ImGui with the Win32 and Direct3D 12 backends only.
add_library(imgui STATIC third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp third_party/imgui/backends/imgui_impl_win32.cpp third_party/imgui/backends/imgui_impl_dx12.cpp)
target_include_directories(imgui PUBLIC third_party/imgui third_party/imgui/backends)
target_compile_definitions(imgui PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)
# Every compute shader is compiled to bytecode by fxc at configure time and embedded
# as a byte array: no D3DCompile and no shader source parsed on a render thread.
if(NOT FXC)
  # CMake cannot read $ENV{ProgramFiles(x86)} (the parenthesis ends the reference),
  # so the usual SDK location is spelled out as well.
  file(GLOB LITEDX12_FXC
    "$ENV{WindowsSdkDir}bin/*/x64/fxc.exe"
    "C:/Program Files (x86)/Windows Kits/10/bin/*/x64/fxc.exe"
    "$ENV{ProgramFiles}/Windows Kits/10/bin/*/x64/fxc.exe")
  if(LITEDX12_FXC)
    list(SORT LITEDX12_FXC COMPARE NATURAL ORDER DESCENDING)
    list(GET LITEDX12_FXC 0 FXC)
  endif()
endif()
if(NOT FXC)
  message(FATAL_ERROR "fxc.exe not found; install the Windows SDK or configure with -DFXC=<path to fxc.exe>")
endif()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
function(litedx12_shader name symbol)
  set(source "${CMAKE_CURRENT_SOURCE_DIR}/src/nr/shaders/${name}.hlsl")
  set(cso "${CMAKE_CURRENT_BINARY_DIR}/generated/${name}.cso")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${source}")
  execute_process(COMMAND "${FXC}" /nologo /T cs_5_0 /E Main /O3 /Fo "${cso}" "${source}"
                  RESULT_VARIABLE result ERROR_VARIABLE error)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "fxc failed on src/nr/shaders/${name}.hlsl: ${error}")
  endif()
  file(READ "${cso}" hex HEX)
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/${name}_shader.hpp"
"#pragma once
// Generated at configure time from src/nr/shaders/${name}.hlsl (fxc /T cs_5_0 /E Main /O3).
// Never edit this file; edit the .hlsl and rebuild.
inline constexpr unsigned char ${symbol}[] = {
${bytes}
};
inline constexpr unsigned int ${symbol}Size = sizeof(${symbol});
")
endfunction()
litedx12_shader(lite_color liteColorShader)
litedx12_shader(lite_preview litePreviewShader)
litedx12_shader(lite_meter liteMeterShader)
# The NR pipeline: the pass, its preview compute shader, white point and the format helpers.
add_library(litedx12_frames STATIC src/nr/gpu_util.cpp src/nr/preview.cpp src/nr/pass.cpp src/nr/white_point.cpp)
target_include_directories(litedx12_frames PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_definitions(litedx12_frames PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(litedx12_frames PRIVATE /W4 /EHsc /utf-8)
target_link_libraries(litedx12_frames PUBLIC d3d11 d3d12 dxgi.lib)
# The plugin body: loader, NGX observer, NR model, command guard, panel. Loaded by dxgi.dll's Start export.
add_library(litedx12_runtime SHARED src/runtime.cpp src/config.cpp src/hooks.cpp src/graphics.cpp src/ui/panel.cpp src/ui/panel_gpu.cpp src/ui/panel_ui.cpp src/ui/ui_widgets.cpp src/input/input.cpp src/input/input_keyboard.cpp src/input/input_directinput.cpp src/nr/caller.cpp src/nr/model_runtime.cpp src/nr/model_settings.cpp src/nr/capture12.cpp src/nr/status.cpp src/nr/ngx_observer.cpp src/nr/command_guard.cpp)
set_target_properties(litedx12_runtime PROPERTIES OUTPUT_NAME "nvngx.dll_litedx12")
target_include_directories(litedx12_runtime PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(litedx12_runtime PRIVATE litedx12_frames detours imgui d3d11 d3d12 dxgi.lib user32 dwmapi dinput8 dxguid advapi32 version)
# Match Opt's loader boundary: loading the body must not initialize D3D12 before
# the game's own startup. Resolve serialization/compiler imports on first use.
target_link_libraries(litedx12_runtime PRIVATE delayimp)
target_link_options(litedx12_runtime PRIVATE /DELAYLOAD:d3d12.dll /DELAYLOAD:D3DCOMPILER_47.dll)
target_compile_definitions(litedx12_runtime PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN "LITE_VERSION=\"${PROJECT_VERSION}\"")
target_compile_options(litedx12_runtime PRIVATE /W4 /EHsc /utf-8)
# Thin DXGI proxy: forwards the system exports and starts the plugin in a thread.
# Every proxy DLL forwards the system exports through the same generated stubs.
# <name>.def is the only place an export is declared: the name table, the jump
# stubs and the resolver table index all come from it at configure time.

# Parse "<export>=Proxy<N> @<ordinal>" lines. Stub numbers must be contiguous and
# in order, because the generated assembly indexes the target table by stub number.
function(litedx12_proxy_def def out_names out_count)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${def}")
  file(STRINGS "${def}" LITE_DEF_LINES)
  set(names "")
  set(slot 0)
  foreach(line IN LISTS LITE_DEF_LINES)
    if(line MATCHES "^[ \t]*([^=# \t]+)=Proxy([0-9]+)[ \t]*@([0-9]+)")
      if(NOT CMAKE_MATCH_2 EQUAL slot)
        message(FATAL_ERROR "${def}: Proxy${CMAKE_MATCH_2} at position ${slot}; stubs must be numbered in order")
      endif()
      list(APPEND names "${CMAKE_MATCH_1}")
      math(EXPR slot "${slot}+1")
    endif()
  endforeach()
  list(LENGTH names count)
  if(count LESS 1)
    message(FATAL_ERROR "No exports parsed from ${def}")
  endif()
  set(${out_names} "${names}" PARENT_SCOPE)
  set(${out_count} "${count}" PARENT_SCOPE)
endfunction()

function(litedx12_proxy_stubs headerDir def resolver table)
  litedx12_proxy_def("${def}" names count)
  file(MAKE_DIRECTORY "${headerDir}")

  set(header "// Generated at configure time from ${def}. Never edit.\n#pragma once\n")
  string(APPEND header "inline constexpr unsigned kProxyExportCount = ${count};\n")
  string(APPEND header "inline constexpr const char* kProxyExportNames[${count}] = {\n")
  foreach(export IN LISTS names)
    string(APPEND header "    \"${export}\",\n")
  endforeach()
  string(APPEND header "};\n")
  file(WRITE "${headerDir}/proxy_exports.generated.hpp" "${header}")

  # Each stub jumps straight to its resolved target; the slow path preserves the
  # integer and both vector argument registers before calling the resolver.
  set(asm "EXTERN ${resolver}:PROC\nEXTERN ${table}:QWORD\n.code\n")
  string(APPEND asm "FORWARD MACRO number\nLOCAL resolved\nProxy&number PROC\n")
  string(APPEND asm " mov rax, QWORD PTR [${table} + number * 8]\n test rax, rax\n jnz resolved\n")
  string(APPEND asm " jmp Slow&number\nresolved:\n jmp rax\nProxy&number ENDP\nSlow&number PROC FRAME\n")
  string(APPEND asm " push rcx\n .allocstack 8\n push rdx\n .allocstack 8\n")
  string(APPEND asm " push r8\n .allocstack 8\n push r9\n .allocstack 8\n sub rsp, 104\n .allocstack 104\n")
  string(APPEND asm " .endprolog\n movdqu [rsp+32], xmm0\n movdqu [rsp+48], xmm1\n movdqu [rsp+64], xmm2\n")
  string(APPEND asm " movdqu [rsp+80], xmm3\n mov ecx, number\n call ${resolver}\n mov r11, rax\n")
  string(APPEND asm " movdqu xmm0, [rsp+32]\n movdqu xmm1, [rsp+48]\n movdqu xmm2, [rsp+64]\n movdqu xmm3, [rsp+80]\n")
  string(APPEND asm " add rsp, 104\n pop r9\n pop r8\n pop rdx\n pop rcx\n jmp r11\nSlow&number ENDP\nENDM\n")
  math(EXPR last "${count}-1")
  foreach(index RANGE ${last})
    string(APPEND asm "FORWARD ${index}\n")
  endforeach()
  string(APPEND asm "END\n")
  file(WRITE "${headerDir}/proxy_stubs.generated.asm" "${asm}")
endfunction()

# Thin DXGI proxy: the 15 system exports keep their real ordinals and are
# forwarded by the generated stubs; the body wraps factories through its own
# detour of the system exports, so the proxy holds no C++ forwarding logic.
set(LITEDX12_DXGI_GEN "${CMAKE_CURRENT_BINARY_DIR}/generated/proxy-dxgi")
litedx12_proxy_stubs("${LITEDX12_DXGI_GEN}" "${CMAKE_CURRENT_SOURCE_DIR}/src/proxy/dxgi.def" ResolveDxgi DxgiTargets)
add_library(dxgi SHARED src/proxy/proxy.cpp "${LITEDX12_DXGI_GEN}/proxy_stubs.generated.asm" src/proxy/dxgi.def)
target_include_directories(dxgi PRIVATE "${LITEDX12_DXGI_GEN}")
add_dependencies(dxgi litedx12_runtime)
target_compile_definitions(dxgi PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(dxgi PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4> $<$<COMPILE_LANGUAGE:CXX>:/EHsc> $<$<COMPILE_LANGUAGE:CXX>:/utf-8>)

# Alternative entry names, OptiScaler style: when another mod already owns
# dxgi.dll, the same body loads as version.dll or winmm.dll instead.
# src/proxy/<name>.def mirrors the system DLL's export names and ordinals; the
# jump stubs and the name table are generated from it at configure time, so
# adding a name means adding one .def plus one call at the bottom.
set(LITEDX12_ENTRY_TARGETS "")
function(litedx12_entry name)
  set(def "${CMAKE_CURRENT_SOURCE_DIR}/src/proxy/${name}.def")
  set(headerDir "${CMAKE_CURRENT_BINARY_DIR}/generated/proxy-${name}")
  file(MAKE_DIRECTORY "${headerDir}")
  litedx12_proxy_stubs("${headerDir}" "${def}" ResolveProxy ProxyTargets)
  file(APPEND "${headerDir}/proxy_exports.generated.hpp"
"inline constexpr const wchar_t* kProxyTargetName = L\"${name}.dll\";
inline constexpr const wchar_t* kProxyOriginalName = L\"${name}-original.dll\";
")

  add_library(entry_${name} SHARED src/proxy/proxy_forward.cpp
              "${headerDir}/proxy_stubs.generated.asm" "${def}")
  set_target_properties(entry_${name} PROPERTIES OUTPUT_NAME "${name}")
  target_include_directories(entry_${name} PRIVATE "${headerDir}")
  add_dependencies(entry_${name} litedx12_runtime)
  target_compile_definitions(entry_${name} PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(entry_${name} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:/W4> $<$<COMPILE_LANGUAGE:CXX>:/EHsc>
    $<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
  set(LITEDX12_ENTRY_TARGETS ${LITEDX12_ENTRY_TARGETS} entry_${name} PARENT_SCOPE)
endfunction()
litedx12_entry(version)
litedx12_entry(winmm)

configure_file(DLSSNR_LiteDX12.ini DLSSNR_LiteDX12.ini COPYONLY)
install(TARGETS dxgi litedx12_runtime ${LITEDX12_ENTRY_TARGETS} RUNTIME DESTINATION .)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/DLSSNR_LiteDX12.ini" DESTINATION .)

