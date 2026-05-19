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

## Build

```sh
./build.sh 1.0.0
```

如果不带参数运行，脚本会提示输入版本号。

## Run

```sh
./build/boardcommd config/boardcomm.conf
```

另开终端：

```sh
LD_LIBRARY_PATH=build ./build/boardcomm_sub demo.topic
LD_LIBRARY_PATH=build ./build/boardcomm_pub demo.topic "hello"
```
