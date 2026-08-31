# LuckyStar — Android ARM64 内存调试工具

> 仅供技术研究与学习，严禁用于非法用途。作者不承担任何违法责任。

> 部署排坑记录见 [TROUBLESHOOTING.md](TROUBLESHOOTING.md)：Git Bash 路径篡改、守护进程被 ps 隐藏、logcat 日志位置、端口占用误判等实际部署问题的原因与解法。

## 项目简介

LuckyStar 是一套 Android ARM64 内存调试工具，包含三个组件：

- **lsdriver** — 内核驱动模块（.ko），提供进程内存读写、内存布局枚举、虚拟触摸/陀螺仪/定位注入、ARM64 硬件断点与 PTE 断点、系统调用监控等能力
- **LS_KTool** — 用户态原生程序，运行在设备上，提供 HTTP RPC 服务器和 ImGui 界面
- **LuckyStarMcp** — Windows 端 MCP 服务器，通过 HTTP 桥接设备，供 AI 客户端调用内存调试工具

---

## 依赖初始化

Capstone、Dear ImGui、nlohmann/json 和 BS::thread_pool 通过 Git 子模块引入。

```bash
git clone --recurse-submodules <repository-url>
```

已有工作区拉取子模块：

```bash
git submodule update --init android/jni/capstone android/jni/imgui android/jni/json android/jni/thread-pool
```

> GitHub 的 Download ZIP 不包含子模块源码，请使用 Git 克隆。

---

## 编译

### 编译 LS_KTool（用户态程序）

使用 CMake + Android NDK 交叉编译：

```bash
# 前置条件：NDK r27d、CMake 3.22+、Ninja
cmake -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=<NDK>/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-26 \
  -DCMAKE_BUILD_TYPE=Release \
  -S android/jni \
  -B build

cmake --build build -j$(nproc)
```

产物：`build/LS_KTool`（ARM aarch64 ELF 可执行文件）

### 编译 Windows 端程序

```bash
cd windows
python build_executables.py
```

产物：`windows/LuckyStar.exe`、`windows/LuckyStarMcp.exe`（PyInstaller 打包）

### 编译内核驱动

在目标内核源码树中执行：

```bash
make -C <KDIR> M=$PWD/lsdriver ARCH=arm64 LLVM=1 modules
```

产物：`lsdriver/lsdriver.ko`

> GKI 内核中部分符号未 `EXPORT_SYMBOL`，驱动通过 `kallsyms_lookup_name` 运行时查找。确保内核未禁用 kprobe。

---

## 使用方法

### 1. 安装内核驱动到设备

```bash
# 推送并加载驱动
adb push lsdriver.ko /data/local/tmp/lsdriver.ko
adb shell "su -c 'insmod /data/local/tmp/lsdriver.ko'"

# 验证
adb shell "lsmod | grep lsdriver"
```

### 2. 启动设备端 HTTP 服务

```bash
# 推送 LS_KTool 到设备
adb push LS_KTool /data/local/tmp/LS_KTool
adb shell "chmod 755 /data/local/tmp/LS_KTool"

# 以模式 4 (HTTP服务器) 启动（参数方式比 stdin 管道可靠）
adb shell "su -c '/data/local/tmp/LS_KTool 4'"

# 进程启动后被驱动隐藏，ps 查不到属预期；日志在 logcat（tag: ls）。
# 验证服务用 curl /health，详见 TROUBLESHOOTING.md
```

服务监听 `0.0.0.0:9494`，提供以下端点：
- `GET /health` — 健康检查
- `POST /api/rpc` — JSON RPC 接口

### 3. 设置 ADB 端口转发

```bash
adb forward tcp:9494 tcp:9494

# 验证
curl http://127.0.0.1:9494/health
# 返回: {"ok":true,"service":"LS_KTool","transport":"http"}
```

### 4. 连接 MCP（ZCode 客户端）

ZCode 配置（`~/.zcode/cli/config.json`）中使用 stdio 模式直接启动 `LuckyStarMcp.exe`：

```json
{
  "mcp": {
    "servers": {
      "luckystar-mcp": {
        "type": "stdio",
        "command": "C:\\path\\to\\windows\\LuckyStarMcp.exe",
        "args": ["--transport", "stdio", "--android-host", "127.0.0.1"],
        "env": {
          "ANDROID_HTTP_HOST": "127.0.0.1",
          "ANDROID_HTTP_PORT": "9494"
        },
        "timeoutMs": 60000
      }
    }
  }
}
```

重启 ZCode 后，`luckystar-mcp` 自动连接。

### 5. 使用 MCP 工具

通过 MCP 可执行的操作示例：

```
# 附加到目标进程
android_target_attach_package(package_name="com.tencent.tmgp.sgame")

# 查看进程内存布局
android_memory_regions()

# 读取内存
android_memory_read(address=0x..., size=16)

# 内存扫描
android_memory_scan_start(mode="unknown")

# 设置断点
android_breakpoint_set(...)

# 查看已保存地址
android_saved_list()
```

### 6. 启动模式说明

LS_KTool 启动时选择模式：

| 模式 | 功能 |
|------|------|
| 0 | 停止驱动线程 |
| 1 | 读写测试 |
| 2 | 触摸测试 |
| 3 | 内存工具（ImGui 界面） |
| 4 | HTTP 服务器 |
| 5 | 陀螺仪测试 |
| 6 | 定位测试 |

---

## 代码结构

### 内核驱动 (`lsdriver/`)

| 文件 | 职责 |
|------|------|
| `lsdriver.c` | 模块入口、连接线程、调度线程、进程退出监听 |
| `io_struct.h` | 共享内存协议定义（操作码、请求结构） |
| `virtual_memory_rw.h` | 软页表遍历 VA→PA 翻译、物理内存读写、跨页处理 |
| `virtual_memory_enum.h` | VMA 枚举、模块收集、扫描区过滤 |
| `virtual_input.h` | 虚拟触摸初始化、slot 劫持、事件上报 |
| `virtual_gyro.h` / `virtual_gnss.h` | 虚拟陀螺仪 / 虚拟定位注入 |
| `arm64_hwdbg.h` | 硬件断点/观察点设置与命中处理 |
| `arm64_ptedbg.h` | PTE 断点（页表 UXN 位数据断点 + 指令模拟执行） |
| `arm64_stepdbg.h` | 单步 PC 断点 |
| `arm64_syscall_monitor.h` | 目标进程系统调用监控（hook `do_el0_svc`） |
| `arm64_cntvct_monitor.h` | CNTVCT_EL0 读取监控（反定时器检测） |
| `break_point.h` | 断点配置框架与统一入口 |
| `inline_hook_frame.h` | ARM64 inline hook 跳板框架 |
| `export_fun.h` | kallsyms 符号查找、内核未导出接口函数指针获取 |
| `arm64_decode/` | ARM64 指令解码器 |
| `arm64_emulate/` | ARM64 指令模拟执行器（含解码/执行缓存） |
| `arm64_encode/` | ARM64 指令编码器 |
| `arm64_ghost/` | 页重定位（模块自身内存搬迁） |
| `arm64_tests/` | decoder/encoder/page relocation 回归测试 |

### 用户态 (`android/jni/`)

| 文件 | 职责 |
|------|------|
| `driver/driver.h` | 内核驱动 C++ 封装（读写、模块查询、断点） |
| `include/http_server.h` | HTTP RPC 服务器（9494 端口） |
| `include/memory_tool.h` | 内存扫描、指针扫描、已保存地址管理 |
| `include/disassembler.h` | Capstone 反汇编封装 |
| `src/main.cpp` | 主程序入口、ImGui 界面 |
| `CMakeLists.txt` | CMake 构建定义 |

### Windows 端 (`windows/`)

| 文件 | 职责 |
|------|------|
| `LuckyStar.py` | PySide6 GUI 客户端 |
| `LuckyStarMcp.py` | MCP 服务器（支持 stdio 和 streamable-http） |
| `http_bridge.py` | Android HTTP 桥接客户端 |
| `build_executables.py` | PyInstaller 打包脚本 |

---

## 共享内存协议

用户态与内核驱动通过固定地址 `0x2025827000` 的共享内存通信。

### 连接流程

1. 用户态进程设置名称为 `LS`（`prctl(PR_SET_NAME)`）
2. 在固定地址 `mmap` 共享内存（`MAP_FIXED_NOREPLACE`）
3. 内核连接线程发现 `LS` 进程，pin 住共享页并 `vmap` 到内核
4. 用户态等待握手完成

### 同步机制

- `kernel` 标志：用户态置 `true` 表示有请求，内核处理前清除
- `user` 标志：内核置 `true` 表示处理完成，用户态等待后清除

### 操作码

| 操作码 | 功能 |
|--------|------|
| `request_op_vmem_read` / `request_op_vmem_write` | 读 / 写进程内存 |
| `request_op_vmem_info` | 枚举进程内存布局 |
| `request_op_touch_init` / `down` / `move` / `up` | 虚拟触摸 |
| `request_op_gyro_init` / `gyro_report` | 虚拟陀螺仪 |
| `request_op_gnss_init` / `gnss_report` | 虚拟定位 |
| `request_op_hwbp_set` / `hwbp_remove` | 硬件断点 |
| `request_op_ptebp_set` / `ptebp_remove` | PTE UXN 断点 |
| `request_op_stepbp_set` / `stepbp_remove` | 单步 PC 断点 |
| `request_op_syscall_monitor_set` / `remove` | 系统调用监控 |
| `request_op_cntvct_monitor_set` / `remove` | CNTVCT_EL0 读取监控 |
| `request_op_env_get_params` | 获取目标 task 环境参数 |
| `request_op_kernel_exit` | 内核线程退出 |

---

## 技术要点

### 内存读写

通过软件页表遍历（PUD 1G / PMD 2M / PTE 4K）将目标进程虚拟地址翻译为物理地址，再经 `phys_to_virt` 内核线性映射（Normal Cacheable）读写物理内存。支持跨页拆分、失败页跳过、mm/PTE 缓存。

### GKI 兼容

GKI 内核中 `copy_from_user_nofault`、`input_class`、`aarch64_insn_patch_text` 等符号未 `EXPORT_SYMBOL`。驱动通过 kprobe 获取 `kallsyms_lookup_name` 地址后运行时查找这些符号，避免 modpost 链接失败。

### inline hook 框架

在模块 `.text` 中预留跳板槽位，通过 `aarch64_insn_patch_text`（`stop_machine` 同步）patch 目标函数入口。跳板保存/恢复全部通用寄存器和 NZCV，支持 PC 重定向。当前用于接管 `breakpoint_handler`、`watchpoint_handler`、`finish_task_switch`、`do_mem_abort`、`brk_handler`、`do_el0_svc`、`cntvct_read_handler`、`cpu_switch_to` 等内核函数。

### 指令模拟执行

`arm64_emulate/` 提供完整的 ARM64 用户态指令模拟器：硬件断点 / PTE 断点命中后，在内核中软件模拟执行当前指令（整数、分支、访存、SIMD/FP），跳过断点指令本身，避免反复陷入。解码与执行结果带缓存以降低热路径开销。
