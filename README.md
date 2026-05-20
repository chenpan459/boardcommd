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

- `libboardcomm.so`：应用侧 SDK
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
  log/        # 统一日志模块

api/          # 应用侧 SDK 源码和对外头文件
examples/     # 示例 / 测试程序
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

应用侧标准接口：

```c
int h = bc_open(NULL);
bc_subscribe_fd(h, "demo.topic");
bc_write(h, "demo.topic", data, len);
bc_write_channel(h, "control", "control.motor", data, len);
ssize_t n = bc_read(h, topic, topic_cap, buf, buf_cap, 1000);
bc_close(h);
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
