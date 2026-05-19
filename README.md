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
