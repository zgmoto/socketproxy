# skproxy

**skproxy** 是一个连接池化的 TCP 中继（TCP proxy with connection pooling）。它预先向上游网关建立大量长连接并保持，客户端请求到达时直接从池中取用已就绪的连接，**消除 TCP 三次握手延迟**，透明双向转发数据。

典型部署：`内网客户端 → skproxy (Client) → [长连接池] → skproxy (Server) → 目标服务`

---

## 原理

### 为什么需要连接池

常规 TCP 代理每次客户端请求都要新建一条到上游的连接——三次握手 + 可能的 TLS 握手会引入固定延迟。skproxy 的做法是：**启动时一次性建立 N 条连接并保持**，请求到达时取一条已就绪的连接直接用，省去建连时间。断开后立即补充，维持池容量。

### 部署拓扑

```
┌──────────┐     ┌─────────────────┐     ┌─────────────────┐     ┌──────────┐
│  客户端   │ ──→ │  skproxy        │ ──→ │  skproxy         │ ──→ │  目标    │
│          │     │  (Client 模式)   │     │  (Server 模式)    │     │  服务    │
└──────────┘     │                 │     │                  │     └──────────┘
                 │ -c remote:9001  │     │ -l 9001          │
                 │ -p 8280:9100   │     │                  │
                 └─────────────────┘     └──────────────────┘
                          │                        │
                          │←── 连接池 (N 条长连接) ──→│
                          │    SOCKS5 认证           │
```

- **Server 模式**（`-l`）：部署在靠近目标服务的一侧，监听端口接受来自 Client skproxy 的池连接，根据端口映射头将流量路由到本地目标端口。
- **Client 模式**（`-c` + `-p`）：部署在靠近客户端的一侧，预先向 Server 建立连接池，同时监听本地端口接受客户端连接。

### 端口映射（2 字节头协议）

Client 与 Server 之间的池连接上使用一个极简的 2 字节路由协议：

1. 客户端连入 Client skproxy 的 `-p listen` 端口
2. Client skproxy 从池中取一条空闲连接，**立即发送 2 字节大端目标端口号**
3. Server skproxy 从池连接读到这 2 字节头，连接 `127.0.0.1:<目标端口>`
4. 之后两端双向透明转发，上层协议完全无感知

```
Client skproxy                         Server skproxy
    │                                       │
    │── [0x20 0x5C] ────────────────────→  │  2 字节头: 目标端口 8284
    │── "GET / HTTP/1.1\r\n..." ────────→  │  后续数据原样转发到 127.0.0.1:8284
    │                                       │
```

头部可能跨 `recv()` 到达——Server 端内部有 2 字节累积缓冲区处理这种边界情况。

### 池连接认证

skproxy Client 与 Server 之间的每条池连接在建立后进行一次 SOCKS5 风格的用户名/密码认证握手：

```
Client                    Server
  │                          │
  │── 0x05 0x01 0x02 ──────→│  方法协商（选用户名/密码）
  │←── 0x05 0x02          ──│  确认
  │                          │
  │── 0x01 ULEN user PLEN pass →│  认证帧
  │←── 0x01 0x00          ──│  成功 → 进入转发模式
```

支持两种凭据来源：
- **用户名/密码**：`-a user:pass`（默认 `admin:admin`）
- **MAC 地址**：`-e eth0`，自动用 `<网卡MAC><4位随机十六进制>` 作为用户名

### 连接池生命周期

```
启动时批量建连（非阻塞 connect）
      │
      ▼
SOCKS5 认证握手（AUTH → PASS → START）
      │
      ▼
就绪等待 ──→ 客户端请求到达 ──→ 配对转发 ──→ 任一端断开
      ▲                                            │
      │                                            ▼
      └────────── 立即补充新连接 ◄─────── 连接释放
```

- **使用过的连接**断开后**立即重建**（`isuse` 标志），保证池容量
- **从未使用的空闲连接**断开后由定时器按需补充

### 内部模型

每个连接是一个 `tcp_conn_t` 节点，两个方向的连接通过 `ext_conn_t` 配对：

```
客户端连接 (way=CLIENT)  ←──配对──→  池连接 (way=SERVER)
     extdata.toconn ─────────────→  extdata.toconn
```

多线程事件循环（`pselect`），每个线程持有独立的 `fd_set`，fd 轮询分配。全局互斥锁保护连接链表的操作。

---

## 使用

### 构建

```bash
make                  # 构建 skproxy（gcc -O2 -lpthread）
make V=1              # 简洁输出
make dir=bin          # 输出到 bin/
make prefix=mips-unknown-linux-uclibc-  # 交叉编译
make clean            # 清理

# 测试工具
cd test && make       # 构建 test/client、test/target
```

### 快速测试

```bash
# 终端 1：启动回显服务器
./test/target 8280

# 终端 2：启动 Server 端 skproxy
./skproxy -l 9001 -a admin:123456

# 终端 3：启动 Client 端 skproxy（映射 8280→9100）
./skproxy -c 127.0.0.1:9001 -a admin:123456 -p 8280:9100

# 终端 4：测试
./test/client 127.0.0.1 9100
```

数据流：`test/client → skproxy(Client):9100 → [池连接+2字节头] → skproxy(Server):9001 → 127.0.0.1:8280 → test/target`

### 命令行

```
skproxy [-c host:port] [-l port] [-p target:listen]... [-a user:pass] [-e dev] [-m num] [-t mode] [-h]
```

| 参数 | 说明 | 默认 |
|------|------|------|
| `-c host:port` | 远端 Server 地址，省略则进入 Server 模式 | — |
| `-l port` | 本地监听端口（Server 模式） | — |
| `-p target:listen` | 端口映射：本地监听 `listen`，Server 端转发到 `127.0.0.1:target`。可多次指定 | — |
| `-a user:pass` | 池连接认证凭据 | `admin:admin` |
| `-e dev` | 网卡名，提取 MAC 生成认证用户名 | `eth0` |
| `-m num` | 连接池大小（Server 模式可设 0 不预建） | `200` |
| `-t print` | 前台测试模式（日志输出到 stdout） | — |
| `-t daemon` | 守护进程模式 | — |
| `-h` | 显示帮助 | — |

### 典型用法

```bash
# Server 模式——部署在靠近目标服务的一侧
skproxy -l 1080 -a myuser:mypass

# Client 模式——部署在靠近客户端的一侧，单端口映射
skproxy -c remote:1080 -a myuser:mypass -p 8280:9100

# 多端口映射——一个 skproxy 实例代理多个目标端口
skproxy -c 10.0.0.1:1080 -p 8280:9100 -p 8281:9101 -p 22:9222

# 使用 MAC 地址自动认证（无需手动指定密码）
skproxy -c 10.0.0.1:40000 -e eth1

# 大连接池（高并发场景）
skproxy -c remote:1080 -p 8280:9100 -m 500
```

### 日志

运行时日志输出到 stdout，同时写入 `/tmp/skproxy.log`（编译时 `DLOG_PRINT` 控制，默认开启）。

```
[2026-06-20 12:00:01] skproxy start, getpid=12345
[2026-06-20 12:00:01] line 238 connect 10.0.0.1:40000, current port=52341, fd=5, total=1
[2026-06-20 12:00:05] line 393: user=admin auth success, fd=5, total=200
[2026-06-20 12:00:06] 10.0.0.1:40000 ==> 192.168.1.100:80, fd=5, 1.50 MB/s
[2026-06-20 12:00:06] 10.0.0.1:40000 <== 192.168.1.100:80, fd=5, 256.3 KB/s
[2026-06-20 12:00:10] close, user=admin, fd=5, total=199
```

- `==>` 客户端→服务器方向，`<==` 服务器→客户端方向
- 速率自动选择 B/s、KB/s、MB/s 单位

### 编译选项

| 宏 | 默认 | 作用 |
|----|------|------|
| `GWLINK_WITH_SOCKS5_PASS` | 定义 | 池连接 SOCKS5 认证 |
| `DLOG_PRINT` | 定义 | 写日志到 `/tmp/skproxy.log` |
| `PTHREAD_SELECT_NUM` | 1 | 事件循环线程数 |

可通过 `make` 时追加 `-D` 覆盖，例如：
```bash
make V=99 D="-DPTHREAD_SELECT_NUM=4"
```

---

## 注意事项

1. **认证层次**：认证仅发生在 skproxy Client ↔ Server 之间。客户端连入 Client skproxy 的 `-p` 端口时无需认证，直接进入转发模式。

2. **MAC 认证**：使用 `-e` 时用户名为 `<12位MAC十六进制><4位随机十六进制>`，密码为空。Server 端需匹配此规则。

3. **守护进程**：`-t daemon` 会 fork + setsid，父进程 `return 1` 是正常行为（非错误）。

4. **错误静默**：对端正常断开（EPIPE/ECONNRESET）不打印错误日志，静默释放连接并补池。

5. **目标不可达**：Server 端无法连接 `-p target` 端口时，释放该池连接，链式关闭对应的 Client 端连接。

6. **缓冲区**：单次 `recv` 缓冲区 8192 字节。对端未就绪时数据在堆上累积（`realloc`），就绪后一次性 flush。极端高并发下需注意内存。
