# 排坑记录 — 小米设备上启动 MCP 服务

部署链路：`lsdriver.ko`（内核）→ `LS_KTool 4`（设备端 HTTP，9494）→ `adb forward` → `LuckyStarMcp.exe`（Windows MCP）。以下均在小米真机（root）实测。

## 1. Git Bash 篡改 adb 设备路径

`adb push ... /data/local/tmp/...` 报 `remote secure_mkdirs() failed`：Git Bash 把 `/data/...` 转成了本地 Windows 路径。涉及设备路径的 adb 命令一律加前缀：

```bash
MSYS_NO_PATHCONV=1 adb -s 26545a06 push lsdriver.ko /data/local/tmp/lsdriver.ko
```

## 2. 启动命令：用参数，别用 stdin 管道

`echo 4 | LS_KTool` 在设备 shell 上不可靠。LS_KTool 支持参数直选模式：

```bash
MSYS_NO_PATHCONV=1 adb -s 26545a06 shell "su -c '/data/local/tmp/LS_KTool 4'"
```

命令立即返回 exit 0 属正常（进程已守护化，脱离会话运行）。

## 3. ps 查不到进程 = 预期行为

驱动握手成功后调用 `hide_task_install(task->tgid)` 隐藏 LS 进程，`ps`/`netstat -p` 均不可见。验证服务用功能面：

```bash
curl http://127.0.0.1:9494/health   # 需先 adb forward
adb shell "su -c 'logcat -d -s ls:V'"
```

## 4. 日志在 logcat，不在 /sdcard/log.txt

日志宏底层是 `__android_log_print`（tag：`ls`），stdout 重定向文件恒为空。成功启动标志：

```
[Driver] 驱动已经连接
[HTTP] 服务端已监听 http://0.0.0.0:9494
```

## 5. errno=98 端口占用 = 重复启动

无端口占用预检，重复启动时旧实例仍在服务、新实例绑定失败退出。看到 errno=98 先 curl `/health`，能通就不用管。

## 6. 握手有 ~2 秒延迟

HTTP 监听在驱动握手完成后才建立，启动后等 2~3 秒再验证。

## 7. USB 重连后 adb forward 规则丢失

设备端守护进程不受 USB 重连影响，但 adb 转发规则会随之失效，表现为 `/health` 无响应。重连后重新执行：

```bash
MSYS_NO_PATHCONV=1 adb -s 26545a06 forward tcp:9494 tcp:9494
```

## 部署清单

```bash
MSYS_NO_PATHCONV=1 adb -s 26545a06 push lsdriver.ko /data/local/tmp/lsdriver.ko
MSYS_NO_PATHCONV=1 adb -s 26545a06 shell "su -c 'insmod /data/local/tmp/lsdriver.ko'"
MSYS_NO_PATHCONV=1 adb -s 26545a06 push build/LS_KTool /data/local/tmp/LS_KTool
MSYS_NO_PATHCONV=1 adb -s 26545a06 shell "su -c 'chmod 755 /data/local/tmp/LS_KTool && /data/local/tmp/LS_KTool 4'"
sleep 3
MSYS_NO_PATHCONV=1 adb -s 26545a06 forward tcp:9494 tcp:9494
curl http://127.0.0.1:9494/health
```

Windows 端 MCP 由 ZCode stdio 配置启动，会话中 `android_bridge_ping` 验证桥接。
