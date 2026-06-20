# CLAUDE.md

本文件为 Claude Code (claude.ai/code) 在此仓库中工作时提供指导。

## 设计目的

**skproxy** 是一个连接池化的 TCP 中继：预先向上游网关建立大量非阻塞 TCP 连接，客户端请求到达时从池中取用已就绪的连接，消除 TCP 建连延迟，透明双向转发数据。新池连接通过 `GWLINK_WITH_SOCKS5_PASS` 执行用户名/密码认证。

典型部署：`内网客户端 → skproxy →[长连接池]→ 公网网关 → 目标服务器`

## 构建

```bash
make              # 构建 skproxy 二进制文件（使用 gcc，链接 -lpthread）
make V=1          # 简洁输出（仅显示状态标签）
make V=99         # 详细输出（完整的编译器命令）
make dir=bin      # 输出到 bin/ 目录
make prefix=mips-unknown-linux-uclibc-  # 交叉编译
make clean        # 删除目标文件和二进制文件
```

测试程序位于 `test/` 目录——顶层 Makefile 不会构建它们：

```bash
cd test && make   # 构建 client、target
```

## 架构

**`skproxy`** 是一个多线程 TCP 代理/网关。它与远程服务器维护一个持久的、非阻塞的 TCP 连接池，并在客户端和服务器之间透明转发流量。池连接间使用 SOCKS5 风格的用户名/密码认证，由编译期宏 `GWLINK_WITH_SOCKS5_PASS` 控制。

### 启动流程 (`main.c`)

1. `start_params()` — 解析命令行参数（`-c host:port`、`-l listen_port`、`-p target:listen`、`-a user:pass`、`-e macdev`、`-m maxconn`、`-t daemon|print`）
2. `process_signal_register()` — 注册 SIGINT/SIGTSTP 信号处理函数，忽略 SIGPIPE
3. `mach_init()` — 清理临时日志文件
4. `select_init()` — 在所有 select 线程中清零 fd_set
5. `net_tcp_connect()` — 向目标服务器建立 `maxconn`（默认 200）个非阻塞 TCP 连接；每个连接启动认证握手
6. `pthread_with_select()` — 创建 `PTHREAD_SELECT_NUM-1` 个子线程 + 主线程，每个线程执行 `select_excute()` → `select_listen()` 循环

### 核心子系统

**连接列表 (`netlist.c/h`)** — `tcp_conn_t` 结构体的单向链表。每个连接跟踪 fd、网关链路状态（`gwlink_status_e` 状态机）、远程主机/端口、缓冲数据，以及一个 `extdata` 指针（类型为 `globals.h` 中的 `ext_conn_t`），用于将两个连接配对成双向隧道。

**Select 循环 (`corecomm.c/h`)** — 事件循环使用 `pselect`，迭代之间休眠 1ms。fd 通过 `select_set()` / `select_wtset()` 在线程间轮询分发（写就绪注册用于非阻塞连接的完成检测）。每个线程检查自己的 `fd_set`；当某个 fd 就绪时，锁定全局互斥锁，调用 `net_tcp_recv(fd)`，然后解锁。

**网络处理器 (`nethandler.c/h`)** — 核心协议层：
- `try_connect()` — 使用 O_NONBLOCK 的非阻塞 `connect()`；立即连接成功时跳过写就绪等待
- `net_tcp_recv()` — 接收端状态机，最复杂的函数。执行路径：
  1. 连接刚完成（异步 connect）：完成设置，发送 SOCKS5 认证问候，加入服务计数
  2. `GWLINK_AUTH` → 处理 SOCKS5 用户名/密码认证（user:pass 或基于 MAC 地址生成）
  3. `GWLINK_PASS` → 检查认证结果；成功则进入 `GWLINK_START`
  4. `GWLINK_START` → 数据转发：如果存在 `extdata->toconn`（双向配对），则使用 `send_with_rate_callback()` 转发；否则，如果是服务器连接且收到数据，从 2 字节头读取目标端口，连接 `127.0.0.1:<target>` 后转发
- `send_with_rate_callback()` — 遇到 EAGAIN 时指数退避发送（100µs → 100ms），如果对端尚未 `GWLINK_START` 则先缓冲数据，之后刷新
- `release_connection_with_fd()` — 关闭 fd，从 select set 中移除，从连接列表中删除，如果连接曾被使用则触发重连

**网关链路状态机 (`netlist.h:gwlink_status_e`)**：
```
GWLINK_INIT → GWLINK_AUTH → GWLINK_PASS → GWLINK_START
                                              ↓
                                        GWLINK_RELEASE
```

### 编译期功能开关

| 宏 | 作用 |
|---|---|
| `GWLINK_WITH_SOCKS5_PASS` | 启用池连接上的用户名/密码认证（在 `globals.h` 中始终定义） |
| `DLOG_PRINT` | 启用文件日志到 `/tmp/skproxy.log`（通过 Makefile 的 `TARGET_DMACRO` 设置） |
| `PTHREAD_SELECT_NUM` | select 线程数（默认 1，可通过 `-D` 覆盖） |
| `TARGET_NAME` | 二进制文件名，通过 Makefile 的 `-D` 传入（默认 `skproxy`） |

### 日志宏 (`dlog.h`)

三个类 printf 宏，可见性各有不同：
- `AI_PRINTF` — 始终输出到 stdout（测试模式除外），当 `DLOG_PRINT` 时同时写文件
- `AO_PRINTF` — 仅在测试模式下输出，当 `DLOG_PRINT` 时始终写文件
- `AT_PRINTF` — 仅在非守护进程模式下输出

### 连接生命周期

服务器连接在连接池中预先建立（`net_tcp_connect`）。当服务器连接有数据到达但尚未配对客户端时，从数据中读取 2 字节目标端口头，然后连接 `127.0.0.1:<target_port>`。一旦配对，数据双向流动。当任一端断开连接时，如果该连接曾被使用（`isuse` 标志），则立即创建一个替换的服务器连接以维持连接池容量。连接池补充也由 `detect_link()` 中基于时间的启发式算法触发。

### 关键默认值 (globals.h)

- 最大连接数：200（`SERVER_TCPLINK_NUM`）
- 默认服务器端口：40000
- 默认认证：admin/admin
- 默认 MAC 设备：eth0
- 缓冲区大小：8192 字节

### 错误处理

- `send()` 遇到 EPIPE/ECONNRESET 时静默返回（对端正常断开），不打印 perror
- Server 端 `try_connect` 目标失败时释放池连接，client 端感知断开
- `time_handler()` 在 server 模式下跳过（无远端可连）

## 运行模式

| 模式 | 命令 | 说明 |
|------|------|------|
| Server | `skproxy -l port [-a auth]` | 中继服务器，接受池连接，通过 2 字节头路由到目标端口 |
| Client | `skproxy -c host:port -p target:listen [-a auth]` | 连接池连远端，每个 `-p` 创建一个本地监听端口 |

## 端口参考

| 参数 | 默认 | 含义 |
|------|------|------|
| `-c host:port` | — | 远端中继服务地址，省略则为 server 模式 |
| `-l port` | — | 池监听端口（server 模式接受来自另一 skproxy 的连接） |
| `-p target:listen` | — | 端口映射：client 监听 `listen`，server 转发到 `127.0.0.1:target`。可多次指定 |
| `-a user:pass` | `admin:admin` | 池连接认证凭据 |
| `-m num` | `200` | 连接池大小（server 模式可设 0） |

## 测试

```bash
# 启动目标回显服务器
./test/target 8280 &

# Server 模式（池监听）
./skproxy -l 9001 -a admin:123456 &

# Client 模式（端口映射）
./skproxy -c 127.0.0.1:9001 -a admin:123456 -p 8280:9100 &

# 原始 TCP 客户端测试
./test/client 127.0.0.1 9100
```

测试辅助工具：
- `test/target` — TCP 回显服务器
- `test/client` — 原始 TCP 客户端（自定义 4 字节头协议）
