cmake_minimum_required(VERSION 3.22)

# ============================================================================
#  Android NDK 工具链 (CMake 原生 Android 平台)
# ----------------------------------------------------------------------------
#  两种用法:
#    1) build_cmake.ps1 传入 -DCMAKE_TOOLCHAIN_FILE=<ndk>/build/cmake/
#       android.toolchain.cmake -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=...
#       (推荐, 走 NDK 官方 toolchain 文件, 与 Android Studio 一致)
#    2) 直接 cmake -S android/jni -B <build-dir>, 此时下面的默认值生效,
#       会被 CMake 自带的 Android-Clang.cmake 自动使用 (不需要 toolchain 文件)。
#  仅当外部未提供 CMAKE_ANDROID_NDK 时填充默认值, 避免覆盖外部传参。
# ============================================================================
if (NOT DEFINED CMAKE_ANDROID_NDK AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_ANDROID_NDK D:/android-ndk-r27d)
    set(CMAKE_SYSTEM_NAME Android)
    set(CMAKE_SYSTEM_VERSION 35)
    set(CMAKE_ANDROID_ARCH_ABI arm64-v8a)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif ()

# ============================================================================
#  NDK 的 flags.cmake (经 Android-Clang.cmake 自动 include) 会向
#  CMAKE_<LANG>_FLAGS 注入若干安全/调试相关 flags。其中以下项对运行时性能有开销,
#  在此精确剔除。保留 -DANDROID -fdata-sections -ffunction-sections
#  -no-canonical-prefixes -Wformat -Werror=format-security (必需或配合 --gc-sections)。
#  用正则匹配独立 token (前后为空格或串首/尾), 避免误伤 -gdwarf / -g3 等。
#    -funwind-tables          生成展开表, 主要增大体积
#    -fstack-protector-strong 每个受保护函数插栈金丝雀, 运行时开销最大
#    -D_FORTIFY_SOURCE=2      memcpy/strcpy 等运行时缓冲区检查插桩
#  注意: 本工程保留 -fexceptions / -frtti (Android.mk 显式开启 -fexceptions,
#        memory_tool.h 依赖 <exception>)。
# ============================================================================
foreach (_lang C CXX ASM)
    string(REGEX REPLACE "(^| )-funwind-tables( |$)" "\\1\\2" CMAKE_${_lang}_FLAGS "${CMAKE_${_lang}_FLAGS}")
    string(REGEX REPLACE "(^| )-fstack-protector-strong( |$)" "\\1\\2" CMAKE_${_lang}_FLAGS "${CMAKE_${_lang}_FLAGS}")
    string(REGEX REPLACE "(^| )-D_FORTIFY_SOURCE=2( |$)" "\\1\\2" CMAKE_${_lang}_FLAGS "${CMAKE_${_lang}_FLAGS}")
    # 清理可能产生的连续空格 / 首尾空格
    string(REGEX REPLACE "  +" " " CMAKE_${_lang}_FLAGS "${CMAKE_${_lang}_FLAGS}")
    string(STRIP "${CMAKE_${_lang}_FLAGS}" CMAKE_${_lang}_FLAGS)
endforeach ()
unset(_lang)

project(LS_KTool LANGUAGES C CXX ASM)

# C++ 标准设置为 C++26 (保留原 Android.mk 的 -std=c++26)。
# 注意: CMake < 3.25 不识别 "CXX26" dialect, 在 project() 期间就会因
# CMAKE_CXX_STANDARD=26 而报错 "requires the language dialect CXX26 ... CMake
# does not know the compile flags"。因此用版本判断: 3.25+ 走标准机制,
# 旧版本显式补 -std=gnu++26 (EXTENSIONS ON 等价, 含 GNU 扩展)。
if (CMAKE_VERSION VERSION_LESS 3.25)
    add_compile_options(-std=gnu++26)
    add_compile_options($<$<COMPILE_LANGUAGE:C>:-std=gnu23>)
else ()
    set(CMAKE_CXX_STANDARD 26)
    set(CMAKE_C_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_C_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS ON)
    set(CMAKE_C_EXTENSIONS ON)
endif ()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# ----------------------------------------------------------------------------
#  编译/链接选项 (对应原 Android.mk 的 -O3 / -fexceptions, Release 启用 LTO)
# ----------------------------------------------------------------------------
add_compile_options(
    $<$<CONFIG:Release>:-O3>
    $<$<CONFIG:Release>:-DNDEBUG>
    $<$<CONFIG:Release>:-flto>
    $<$<CONFIG:Release>:-fomit-frame-pointer>

    $<$<CONFIG:Release>:-ffunction-sections>
    $<$<CONFIG:Release>:-fdata-sections>
    $<$<CONFIG:Release>:-funroll-loops>

    $<$<CONFIG:Debug>:-O0>
    $<$<CONFIG:Debug>:-g3>
    $<$<CONFIG:Debug>:-fno-omit-frame-pointer>

    # 对应 Android.mk 的 LOCAL_CPPFLAGS += -fexceptions (开启 C++ 异常)
    -fexceptions
)
add_link_options(
    $<$<CONFIG:Release>:-Wl,--gc-sections>
    $<$<CONFIG:Release>:-flto>
)

# ============================================================================
#  Capstone 反汇编引擎 (静态库) —— 仅编译核心 + AArch64 架构
#  对应原 Android.mk 5-22 行。不使用 add_subdirectory(capstone),
#  因为 capstone 自带 CMake 会拉全架构, 手动 glob 更贴近原 ndk-build 行为且产物更小。
# ============================================================================
# 子模块未初始化时给出清晰错误, 避免下方 add_library 报晦涩的 "No SOURCES"
if (NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/capstone/include/capstone/capstone.h)
    message(FATAL_ERROR
        "capstone 子模块未检出。请先执行:\n"
        "  git submodule update --init android/jni/capstone")
endif ()
file(GLOB CAPSTONE_CORE_SRC ${CMAKE_CURRENT_SOURCE_DIR}/capstone/*.c)
file(GLOB CAPSTONE_ARCH_SRC ${CMAKE_CURRENT_SOURCE_DIR}/capstone/arch/AArch64/*.c)
add_library(capstone STATIC ${CAPSTONE_CORE_SRC} ${CAPSTONE_ARCH_SRC})
target_include_directories(capstone PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/capstone/include)
target_compile_options(capstone PRIVATE -O3 -w -std=c99)
target_compile_definitions(capstone PRIVATE
    CAPSTONE_HAS_AARCH64
    CAPSTONE_USE_SYS_DYN_MEM
)

# ============================================================================
#  Dear ImGui (静态库) —— 核心源文件 + Android/OpenGL3/Vulkan 后端
#  对应原 Android.mk 94-97 行。不引入 volk, 走 imgui 非 volk 默认路径
#  (Android.mk 原本只 -lvulkan, 不定义 IMGUI_IMPL_VULKAN_USE_VOLK)。
# ============================================================================
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/imgui)
if (NOT EXISTS ${IMGUI_DIR}/imgui.cpp)
    message(FATAL_ERROR
        "imgui 子模块未检出。请先执行:\n"
        "  git submodule update --init android/jni/imgui")
endif ()
add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_android.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
    ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp
)
target_compile_definitions(imgui PRIVATE
    VK_USE_PLATFORM_ANDROID_KHR
)
target_include_directories(imgui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

# ============================================================================
#  LS_KTool 主程序 (可执行文件)
#  对应原 Android.mk 26-108 行。
# ============================================================================
# 头文件型子模块检查 (json / thread-pool 仅 include, 不编译)
if (NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/json/single_include/nlohmann/json.hpp)
    message(FATAL_ERROR
        "json 子模块未检出。请先执行:\n"
        "  git submodule update --init android/jni/json")
endif ()
if (NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/thread-pool/include/BS_thread_pool.hpp)
    message(FATAL_ERROR
        "thread-pool 子模块未检出。请先执行:\n"
        "  git submodule update --init android/jni/thread-pool")
endif ()

# 默认开启 LS_LOGD/LS_LOGD_TAG 调试日志 (对应原 Android.mk 的 LS_DEBUG_LOG ?= 1)。
# 可执行 cmake -DLS_DEBUG_LOG=0 关闭。
set(LS_DEBUG_LOG 1 CACHE STRING "Enable LS_LOGD debug logging (1=on, 0=off)")

# 主程序源文件 (对应 FILE_LIST)
set(LS_KTOOL_SOURCES
    src/main.cpp
    src/cloudflared_blob.S
    # imgui 核心与后端通过链接 imgui 静态库引入, 此处不再重复列
)

add_executable(LS_KTool ${LS_KTOOL_SOURCES})

target_compile_definitions(LS_KTool PRIVATE
    LS_DEBUG_LOG=${LS_DEBUG_LOG}
    VK_USE_PLATFORM_ANDROID_KHR
)

# 强制包含 logger.h —— 对应 Android.mk 的
# LOCAL_CPPFLAGS += -include $(SHARED_JNI_ROOT)/include/logger.h
# (SHARED_JNI_ROOT 在 ndk-build 下即 jni 目录, 等价于此处)
# 注意: 必须用 $<COMPILE_LANGUAGE:CXX> 限定, 否则 -include 也会传给
# cloudflared_blob.S 的汇编器, 导致汇编器试图预处理 C 头文件而报错
# (原 Android.mk 的 LOCAL_ASFLAGS 与 LOCAL_CPPFLAGS 是分开的, -include 只在后者)。
target_compile_options(LS_KTool PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-include>
    $<$<COMPILE_LANGUAGE:CXX>:${CMAKE_CURRENT_SOURCE_DIR}/include/logger.h>
)

# cloudflared_blob.S 用 .incbin "assets/cloudflared-linux-arm64" 引用资源,
# 相对路径以 jni 目录为根。clang 汇编 .S 文件时的工作目录即源文件所在,
# 但 .incbin 的搜索路径默认是 -I 传入的 include 目录。这里只给汇编文件
# 加 jni 根目录作为 include 路径 (对应 Android.mk 的 LOCAL_ASFLAGS += -I$(LOCAL_PATH)),
# 不继承 target 的其他 -I (避免汇编器解析系统头)。
set_source_files_properties(src/cloudflared_blob.S PROPERTIES
    INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}
)

# 头文件搜索路径 (对应 Android.mk 的 LOCAL_C_INCLUDES)
target_include_directories(LS_KTool PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/driver
    ${CMAKE_CURRENT_SOURCE_DIR}/font
    ${CMAKE_CURRENT_SOURCE_DIR}/android_draw
    ${CMAKE_CURRENT_SOURCE_DIR}/android_imgui_input
    ${CMAKE_CURRENT_SOURCE_DIR}/android_surface
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui/backends
    ${CMAKE_CURRENT_SOURCE_DIR}/json/single_include
    ${CMAKE_CURRENT_SOURCE_DIR}/thread-pool/include
)

# 链接 (对应 Android.mk 的 LOCAL_STATIC_LIBRARIES + LOCAL_LDLIBS)
target_link_libraries(LS_KTool PRIVATE
    capstone
    imgui
    # LDLIBS: -lvulkan -landroid -llog -lEGL -lGLESv3
    vulkan android log EGL GLESv3
    # 系统库 (c++_static STL 由工具链默认提供, 这里补 POSIX/数学依赖)
    m dl z
)

# ============================================================================
#  adb 推送目标 —— 对应原 Android.mk 111-124 行的 push-akernel
#  构建 Release 后执行: cmake --build <build-dir> --target push-akernel
#  将 LS_KTool push 到 /data/local/tmp 再 su -c cp 到 /data/akernel
# ============================================================================
set(AKERNEL_PUSH_DIR /data/akernel)
set(AKERNEL_TEMP_FILE /data/local/tmp/LS_KTool)
set(AKERNEL_REMOTE_FILE ${AKERNEL_PUSH_DIR}/LS_KTool)
set(AKERNEL_REMOTE_NEW_FILE ${AKERNEL_REMOTE_FILE}.new)

add_custom_target(push-akernel
    COMMAND ${CMAKE_COMMAND} -E echo "Pushing LS_KTool to device..."
    COMMAND adb push $<TARGET_FILE:LS_KTool> ${AKERNEL_TEMP_FILE}
    COMMAND adb shell su -c "mkdir -p ${AKERNEL_PUSH_DIR} && cp ${AKERNEL_TEMP_FILE} ${AKERNEL_REMOTE_NEW_FILE} && chmod 755 ${AKERNEL_REMOTE_NEW_FILE} && mv -f ${AKERNEL_REMOTE_NEW_FILE} ${AKERNEL_REMOTE_FILE}"
    DEPENDS LS_KTool
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "adb push LS_KTool -> ${AKERNEL_REMOTE_FILE}"
    VERBATIM
)
