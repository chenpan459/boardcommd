# boardcommd
板间通信总线。

本工程采用：

```text
Applications
  -> SDK API
  -> Message Bus
  -> Router
  -> Protocol Layer
  -> Transport Interface
  -> TCP / UART / UDP Plugin
```

当前 MVP 使用 C 语言实现：

- `libbc_api.so`：应用侧 SDK
- `bc_open/read/write/close`：应用侧标准读写接口
- `boardcommd`：独立通信守护进程
- `MessageBus`：publish / subscribe
- `Router`：topic -> transport
- `Protocol`：统一 frame、seq、checksum 基础封装
- `Transport`：UDP 可运行，TCP/UART 插件骨架已接入

## Layout

```text
src/
  daemon/     # boardcommd 服务进程
  transport/  # TCP / UDP / UART 插件
  log/        # 日志实现（头文件在 api/）

api/          # 应用侧 SDK 对外头文件与库源码
examples/     # 示例 / 测试程序
```

## 第三方应用集成

`api/` 目录即为对外 SDK，编译应用时只需包含该目录下的头文件：

```text
api/bc.h              # 核心 API（必需）
api/bc_types.h        # 公共类型（由 bc.h 引入）
api/bc_log.h          # 可选日志辅助（示例程序使用）
```

链接：

```text
-lbc_api                 # 核心库（bc_open / bc_write / bc_read 等）
-lboardcomm_log          # 可选，仅在使用 bc_log_* / BC_LOGI 时需要
```

最小示例（不依赖日志库）：

```c
#include "bc.h"

int main(void) {
    int h = bc_open(NULL);
    bc_write(h, "demo.topic", "hello", 5);
    bc_close(h);
    return 0;
}
```

编译示例：

```sh
gcc -I/path/to/boardcommd/api app.c -L/path/to/build -lbc_api
```

若使用 `bc_log.h`：

```sh
gcc -I/path/to/boardcommd/api app.c -L/path/to/build -lbc_api -lboardcomm_log
```

## Build

```sh
./build.sh 1.0.0
```

不带版本号运行会显示正确用法并退出。

## Run

```sh
./build/boardcommd config/boardcomm.conf
```

另开终端：

```sh
LD_LIBRARY_PATH=build ./build/boardcomm_sub demo.topic
LD_LIBRARY_PATH=build ./build/boardcomm_pub demo.topic "hello"
```

文件收发测试（**必须先启动接收端，并在超时前启动发送端**）：

```sh
# 终端 1：先启动，看到 "ready ... start file_send now" 后再操作终端 2
LD_LIBRARY_PATH=build ./build/boardcomm_file_recv file.topic /tmp/boardcomm_recv.bin 300000

# 终端 2：大文件也需在接收端等待期间发送
LD_LIBRARY_PATH=build ./build/boardcomm_file_send file.topic /path/to/test.tar.gz

# 校验
diff /path/to/test.tar.gz /tmp/boardcomm_recv.bin
```

小文件快速验证：

```sh
echo "boardcomm file test" > /tmp/boardcomm_send.txt
LD_LIBRARY_PATH=build ./build/boardcomm_file_recv file.topic /tmp/boardcomm_recv.bin 60000
LD_LIBRARY_PATH=build ./build/boardcomm_file_send file.topic /tmp/boardcomm_send.txt
diff /tmp/boardcomm_send.txt /tmp/boardcomm_recv.bin
```

参数含义：

```text
boardcomm_file_recv [topic] [save_path] [timeout_ms] [channel]
boardcomm_file_send [topic] <file_path> [channel]
```

`timeout_ms` 为等待**第一包**的最长时间；大文件建议 `300000`（5 分钟）或更长。`rc=-6` 表示超时（常见原因：发送端启动太晚）。

文件分块会同时投递本地订阅者，并按 `route` / `channel` 走网络传输（与跨板一致）。同机联调也会经过 `route *` 等配置；跨板时在 `boardcomm.conf` 中把对端 IP/端口配到对应 `transport`，或使用 `bc_file_send_channel` / `bc_file_recv_channel` 指定 channel。

应用侧标准接口：

```c
int h = bc_open(NULL);
bc_subscribe_fd(h, "demo.topic");
bc_write(h, "demo.topic", data, len);
bc_write_channel(h, "control", "control.motor", data, len);
ssize_t n = bc_read(h, topic, topic_cap, buf, buf_cap, 1000);
bc_close(h);
```

文件传输（基于 topic 分块 publish，单文件最大 4 GiB）：

```c
int h = bc_open(NULL);
bc_file_recv(h, "file.topic", "/tmp/recv.bin", 30000);  /* 先订阅并等待 */
/* 对端: bc_file_send(h, "file.topic", "/path/to/file"); */
bc_close(h);
```

带 channel 路由：

```c
bc_file_send_channel(h, "control", "file.topic", "/path/to/file");
bc_file_recv_channel(h, "control", "file.topic", "/tmp/recv.bin", 30000);
```

配置文件支持多通道和多路由：

```text
transport udp0 udp 127.0.0.1:9101 9100 0
transport udp1 udp 127.0.0.1:9201 9200 0

channel control udp0
channel telemetry udp1

route control.* udp0
route telemetry.* udp1
route * udp0
```

`transport` 配置格式：

```text
transport <name> <type> <remote_ip:remote_port> <local_port> <baudrate>
```

例如：

```text
transport udp0 udp 127.0.0.1:9101 9100 0
```

含义：

```text
udp0            transport 名称
udp             transport 类型
127.0.0.1:9101 远端目的 IP 和目的端口
9100            本地监听 / 绑定端口
0               波特率，UDP/TCP 不使用，填 0
```

两块板部署时，本板如果要向 `192.168.1.20:9101` 发送，可以配置：

```text
transport udp0 udp 192.168.1.20:9101 9100 0
```

对端板如果要回发，则对端配置里的 `remote_ip:remote_port` 应该指向本板 IP 和本板监听端口。

## Test Examples

性能吞吐量 / 传输速率测试：

```sh
LD_LIBRARY_PATH=build ./build/boardcomm_perf_sub perf.topic 10
LD_LIBRARY_PATH=build ./build/boardcomm_perf_pub perf.topic 10 1024
```

参数含义：

```text
boardcomm_perf_sub [topic] [duration_sec]
boardcomm_perf_pub [topic] [duration_sec] [payload_size]
```

稳定性发送测试：

```sh
LD_LIBRARY_PATH=build ./build/boardcomm_perf_sub stable.topic
LD_LIBRARY_PATH=build ./build/boardcomm_stability_pub stable.topic 3600 256 10000
```

参数含义：

```text
boardcomm_stability_pub [topic] [duration_sec] [payload_size] [interval_us]
```

日志输出到控制台，同时写入 `log/*.log`。
