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
- `libbc_transport.so`：传输层插件（UDP / TCP / UART）
- `boardcommd`：独立通信守护进程
- `MessageBus`：publish / subscribe
- `Router`：topic -> transport
- `Protocol`：统一 frame、seq、checksum 基础封装
- `Transport`：UDP 可运行，TCP/UART 插件骨架已接入

## Layout

```text
src/
  daemon/     # boardcommd 服务进程
  transport/  # UDP / TCP / UART 插件（libbc_transport.so）
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

TCP 必须明确一端监听、一端连接；两端不能都配成 client，也不能都 listen 同一端口。

推荐用第 6 个字段写角色（`server` / `client`，也支持 `listen` / `connect`）：

```text
# 板 A：监听
transport tcps0 tcp 0.0.0.0:9300 9300 0 server
# 板 B：连接板 A
transport tcpc0 tcp 192.168.1.10:9300 0 0 client
```

- **server**：`endpoint` 为 bind 地址；监听端口优先用 `local_port`，若为 0 则用 `endpoint` 里的端口。
- **client**：`endpoint` 为要连接的 `host:port`；`local_port` 忽略。

未写角色时保持兼容：`local_port > 0` 视为 server，否则为 client（易与 UDP 的 `local_port` 语义混淆，两块板对接时务必写 `server`/`client`）。

额外配置项：

```text
node_id 1
socket /tmp/boardcommd.sock
socket_uid 0          # 非 0 时启用 Unix socket SO_PEERCRED 校验
require_route         # 无匹配 route 时 publish 返回错误
no_bridge             # 关闭 dst_node=0 的网络桥接转发
plugin /path/to/libcustom.so
```

## 能力清单（P0–P3）

| 级别 | 能力 |
|------|------|
| P0 | TCP 监听/连接状态机、`node_id` 写入帧、`handle_inbound` 网络桥接/转发、publish 路由/发送失败上报 |
| P1 | QoS1 ACK、分片重组（>1392B）、订阅通配符 `telemetry.*`、SHM 本地 IPC 快路径 |
| P2 | 配置校验、transport `dlopen` 插件、daemon 统计、`SO_PEERCRED` 鉴权、SIGINT/SIGTERM 优雅退出、UART `tcdrain`、单元测试 |
| P3 | `bc_context_*` 多实例、`bc_write_ex` 扩展发布、`bc_discover_nodes`/`bc_persist_enable` 桩 API |

扩展 API 示例：

```c
bc_publish_opts_t opts = {.dst_node = 2, .qos = BC_QOS_AT_LEAST_ONCE};
bc_write_ex(h, NULL, "telemetry.temp", &temp, sizeof(temp), &opts);

bc_enable_shm(h);  /* 或 bc_context_enable_shm(ctx) */

bc_client_stats_t stats;
bc_get_stats(h, &stats);

bc_context_t *ctx = bc_context_create(NULL);
bc_context_enable_shm(ctx);
bc_context_publish(ctx, "demo.topic", "hi", 2);
bc_context_destroy(ctx);
```

运行单元测试：

```sh
./build/test_topic_match
```


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
