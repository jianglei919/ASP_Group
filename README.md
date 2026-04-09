# ASP_Group — 分布式文件检索系统

## 1. 项目概述

基于 Socket 的分布式文件检索系统，采用 **1 主 + 2 镜像** 的三节点架构：

| 组件 | 端口 | 职责 |
|---|---|---|
| `w26server` | 5000 | 主服务端：接入入口、全局序号分配、自身序号段的业务处理 |
| `mirror1` | 5001 | 镜像节点 1：独立业务处理，通过心跳向主服上报在线状态 |
| `mirror2` | 5002 | 镜像节点 2：同 mirror1 |
| `client` | — | 客户端：连接主服获取路由，跟随 REDIRECT 到目标节点 |

三个服务节点都具备**完整且对等的业务处理能力**（dirlist / fn / fz / ft / fdb / fda），不依赖主服代理转发。

## 2. 系统架构

```mermaid
graph TB
    subgraph clients ["客户端"]
        style clients fill:#E3F2FD,stroke:#1565C0,stroke-width:2px
        C1["client #1-2"]
        C2["client #3-4"]
        C3["client #5-6"]
        C4["client #7+<br/>(循环轮转)"]
    end

    subgraph cluster ["服务端集群"]
        style cluster fill:#FFF8E1,stroke:#F9A825,stroke-width:2px
        W26["w26server<br/>:5000<br/>主服务端 + 业务节点"]
        M1["mirror1<br/>:5001<br/>镜像业务节点"]
        M2["mirror2<br/>:5002<br/>镜像业务节点"]
    end

    C1 -->|"CONNECT_PROBE → CONNECTED"| W26
    C2 -->|"CONNECT_PROBE → REDIRECT"| W26
    C2 -.->|"重连"| M1
    C3 -->|"CONNECT_PROBE → REDIRECT"| W26
    C3 -.->|"重连"| M2
    C4 -->|"按序号轮转"| W26

    M1 -->|"HEARTBEAT<br/>每2秒"| W26
    M2 -->|"HEARTBEAT<br/>每2秒"| W26

    style W26 fill:#FFCC80,stroke:#E65100,stroke-width:2px,color:#000
    style M1 fill:#A5D6A7,stroke:#2E7D32,stroke-width:2px,color:#000
    style M2 fill:#CE93D8,stroke:#6A1B9A,stroke-width:2px,color:#000
    style C1 fill:#90CAF9,stroke:#1565C0,stroke-width:1px,color:#000
    style C2 fill:#90CAF9,stroke:#1565C0,stroke-width:1px,color:#000
    style C3 fill:#90CAF9,stroke:#1565C0,stroke-width:1px,color:#000
    style C4 fill:#90CAF9,stroke:#1565C0,stroke-width:1px,color:#000
```

## 3. 连接序号分配规则

客户端连接序号由 `w26server` 通过原子文件计数器 (`/tmp/w26_client_seq.txt`) 分配，**心跳连接不消耗序号**。

| 序号 | 归属节点 | 说明 |
|---|---|---|
| 1-2 | w26server | 主服务端本地处理 |
| 3-4 | mirror1 | 重定向到 mirror1 |
| 5-6 | mirror2 | 重定向到 mirror2 |
| 7, 10, 13, 16... | w26server | 循环轮转 (seq-7) % 3 == 0 |
| 8, 11, 14, 17... | mirror1 | 循环轮转 (seq-7) % 3 == 1 |
| 9, 12, 15, 18... | mirror2 | 循环轮转 (seq-7) % 3 == 2 |

序号在 `w26server` 启动时清零，运行期间只增不减。

```mermaid
flowchart TD
    START(["新客户端连接"]) --> PROBE["发送 CONNECT_PROBE"]
    PROBE --> SEQ["w26server 原子分配序号 seq"]
    SEQ --> CHECK{seq <= 6 ?}

    CHECK -->|"是"| FIXED{"seq 落在哪个区间?"}
    FIXED -->|"1-2"| R0["route_index = 0<br/>w26server"]
    FIXED -->|"3-4"| R1["route_index = 1<br/>mirror1"]
    FIXED -->|"5-6"| R2["route_index = 2<br/>mirror2"]

    CHECK -->|"否 (seq >= 7)"| MOD["计算 (seq - 7) % 3"]
    MOD -->|"== 0"| R0
    MOD -->|"== 1"| R1
    MOD -->|"== 2"| R2

    R0 --> LOCAL["回复 CONNECTED<br/>本地处理"]
    R1 --> ONLINE1{"mirror1 在线?"}
    R2 --> ONLINE2{"mirror2 在线?"}

    ONLINE1 -->|"是"| REDIR1["回复 REDIRECT<br/>→ mirror1:5001"]
    ONLINE1 -->|"否"| LOCAL
    ONLINE2 -->|"是"| REDIR2["回复 REDIRECT<br/>→ mirror2:5002"]
    ONLINE2 -->|"否"| LOCAL

    style START fill:#E3F2FD,stroke:#1565C0,stroke-width:2px,color:#000
    style SEQ fill:#FFF9C4,stroke:#F9A825,stroke-width:2px,color:#000
    style R0 fill:#FFCC80,stroke:#E65100,stroke-width:2px,color:#000
    style R1 fill:#A5D6A7,stroke:#2E7D32,stroke-width:2px,color:#000
    style R2 fill:#CE93D8,stroke:#6A1B9A,stroke-width:2px,color:#000
    style LOCAL fill:#FFCC80,stroke:#E65100,stroke-width:1px,color:#000
    style REDIR1 fill:#A5D6A7,stroke:#2E7D32,stroke-width:1px,color:#000
    style REDIR2 fill:#CE93D8,stroke:#6A1B9A,stroke-width:1px,color:#000
    style ONLINE1 fill:#C8E6C9,stroke:#2E7D32,stroke-width:1px,color:#000
    style ONLINE2 fill:#E1BEE7,stroke:#6A1B9A,stroke-width:1px,color:#000
    style CHECK fill:#FFF9C4,stroke:#F9A825,stroke-width:1px,color:#000
    style FIXED fill:#FFF9C4,stroke:#F9A825,stroke-width:1px,color:#000
    style MOD fill:#FFF9C4,stroke:#F9A825,stroke-width:1px,color:#000
    style PROBE fill:#BBDEFB,stroke:#1565C0,stroke-width:1px,color:#000
```

## 4. 客户端连接时序

### 4.1 归属 w26server（本地处理）

```mermaid
sequenceDiagram
    participant C as 🔵 client
    participant W as 🟠 w26server

    C->>W: TCP connect (:5000)
    C->>W: CONNECT_PROBE
    Note over W: 分配 seq=1, route_index=0<br/>归属本地
    W-->>C: CONNECTED w26server 127.0.0.1 5000
    Note over C: 显示 "NODE: w26server"

    rect rgb(240, 248, 255)
        Note over C,W: 业务会话
        C->>W: dirlist -a
        W-->>C: dir1 dir2 dir3 ...
        C->>W: fn sample.txt
        W-->>C: filename=sample.txt size=1024 ...
        C->>W: quitc
        W-->>C: BYE
    end
```

### 4.2 归属 mirror（REDIRECT 重连）

```mermaid
sequenceDiagram
    participant C as 🔵 client
    participant W as 🟠 w26server
    participant M as 🟢 mirror1

    C->>W: TCP connect (:5000)
    C->>W: CONNECT_PROBE
    Note over W: 分配 seq=3, route_index=1<br/>归属 mirror1, 检查心跳在线
    W-->>C: REDIRECT 127.0.0.1 5001
    Note over C: 断开与 w26server 的连接

    C->>M: TCP connect (:5001)
    C->>M: CONNECT_PROBE
    M-->>C: CONNECTED mirror1 127.0.0.1 5001
    Note over C: 显示 "NODE: mirror1"

    rect rgb(240, 255, 240)
        Note over C,M: 业务会话（直接与 mirror1 通信）
        C->>M: fz 100 10000
        M-->>C: FILE 2048 + [二进制tar.gz]
        C->>M: quitc
        M-->>C: BYE
    end
```

### 4.3 心跳上报

```mermaid
sequenceDiagram
    participant M1 as 🟢 mirror1
    participant W as 🟠 w26server
    participant M2 as 🟣 mirror2

    loop 每 2 秒
        M1->>W: TCP connect
        M1->>W: HEARTBEAT mirror1
        Note over W: 更新 mirror1 时间戳<br/>（不消耗客户端序号）
        W-->>M1: HB_OK
        M1->>W: 断开连接
    end

    loop 每 2 秒
        M2->>W: TCP connect
        M2->>W: HEARTBEAT mirror2
        Note over W: 更新 mirror2 时间戳
        W-->>M2: HB_OK
        M2->>W: 断开连接
    end

    Note over W: GET_NODES 查询时<br/>心跳时间差 <= 6s → 在线<br/>心跳时间差 > 6s → 离线
```

## 5. 通信协议

```mermaid
graph LR
    subgraph control ["控制协议"]
        style control fill:#E8F5E9,stroke:#2E7D32,stroke-width:2px
        CP["CONNECT_PROBE"] --> CONN["CONNECTED name host port"]
        CP --> REDIR["REDIRECT host port"]
        HB["HEARTBEAT nodeName"] --> HBOK["HB_OK / HB_ERR"]
        GN["GET_NODES"] --> NS["NODES w26server=1 mirror1=N mirror2=N"]
        PING["PING"] --> PONG["PONG nodeName"]
        QC["quitc"] --> BYE["BYE"]
    end

    subgraph business ["业务协议"]
        style business fill:#E3F2FD,stroke:#1565C0,stroke-width:2px
        DL["dirlist -a / -t"] --> TEXT["文本行响应"]
        FN["fn filename"] --> META["filename=... size=... created=... permissions=..."]
        FZ["fz size1 size2"] --> FILE["FILE size + 二进制tar.gz"]
        FT["ft ext1 ..."] --> FILE
        FDB["fdb YYYY-MM-DD"] --> FILE
        FDA["fda YYYY-MM-DD"] --> FILE
    end

    style CP fill:#FFCC80,stroke:#E65100,color:#000
    style HB fill:#FFCC80,stroke:#E65100,color:#000
    style GN fill:#FFCC80,stroke:#E65100,color:#000
    style PING fill:#FFCC80,stroke:#E65100,color:#000
    style QC fill:#FFCC80,stroke:#E65100,color:#000
    style DL fill:#90CAF9,stroke:#1565C0,color:#000
    style FN fill:#90CAF9,stroke:#1565C0,color:#000
    style FZ fill:#90CAF9,stroke:#1565C0,color:#000
    style FT fill:#90CAF9,stroke:#1565C0,color:#000
    style FDB fill:#90CAF9,stroke:#1565C0,color:#000
    style FDA fill:#90CAF9,stroke:#1565C0,color:#000
```

## 6. w26server 子进程处理流程

`w26server` 采用 `fork-per-connection` 并发模型。每个 TCP 连接由一个独立子进程处理，子进程通过读取第一条命令来区分心跳连接与客户端连接：

```mermaid
flowchart TD
    ACCEPT(["accept() 新连接"]) --> FORK["fork() 子进程"]
    FORK --> READ["读取第一条命令"]

    READ --> IS_HB{"以 HEARTBEAT 开头?"}

    IS_HB -->|"是"| HB_PROC["更新心跳时间戳<br/>回复 HB_OK"]
    HB_PROC --> EXIT1(["子进程退出<br/>❌ 不消耗序号"])

    IS_HB -->|"否"| ALLOC["调用 next_client_seq()<br/>原子分配序号"]
    ALLOC --> ROUTE["preferred_index_by_seq()<br/>计算 route_index"]
    ROUTE --> CMD_LOOP{"命令循环"}

    CMD_LOOP -->|"CONNECT_PROBE"| DECIDE{"route_index == 0?"}
    DECIDE -->|"本地"| CONNECTED["回复 CONNECTED"]
    DECIDE -->|"mirror"| CHECK_ONLINE{"mirror 在线?"}
    CHECK_ONLINE -->|"是"| REDIRECT["回复 REDIRECT"]
    CHECK_ONLINE -->|"否"| CONNECTED

    CMD_LOOP -->|"业务命令"| BIZ["execute_local 或 REDIRECT"]
    CMD_LOOP -->|"quitc"| BYE_RESP["回复 BYE"]
    CMD_LOOP -->|"连接断开"| EXIT2(["子进程退出"])
    BYE_RESP --> EXIT2

    CONNECTED --> CMD_LOOP
    BIZ --> CMD_LOOP

    style ACCEPT fill:#E3F2FD,stroke:#1565C0,stroke-width:2px,color:#000
    style FORK fill:#E3F2FD,stroke:#1565C0,stroke-width:1px,color:#000
    style IS_HB fill:#FFF9C4,stroke:#F9A825,stroke-width:2px,color:#000
    style HB_PROC fill:#C8E6C9,stroke:#2E7D32,stroke-width:1px,color:#000
    style EXIT1 fill:#FFCDD2,stroke:#C62828,stroke-width:1px,color:#000
    style ALLOC fill:#FFCC80,stroke:#E65100,stroke-width:2px,color:#000
    style ROUTE fill:#FFCC80,stroke:#E65100,stroke-width:1px,color:#000
    style CONNECTED fill:#A5D6A7,stroke:#2E7D32,stroke-width:1px,color:#000
    style REDIRECT fill:#CE93D8,stroke:#6A1B9A,stroke-width:1px,color:#000
    style EXIT2 fill:#FFCDD2,stroke:#C62828,stroke-width:1px,color:#000
```

## 7. 项目结构

```text
ASP_Group/
├── src/
│   ├── w26server.c          # 主服务端（接入分流 + 业务处理）
│   ├── mirror1.c            # 镜像节点 1（心跳 + 业务处理）
│   ├── mirror2.c            # 镜像节点 2（心跳 + 业务处理）
│   └── client.c             # 客户端（CONNECT_PROBE + 命令交互）
├── scripts/
│   ├── start_all_servers.sh  # 一键启动三个服务端（支持 --root / --depth）
│   ├── stop_all_servers.sh   # 一键停止
│   ├── server_status.sh      # 查看运行状态
│   ├── run_w26server.sh      # 单独启动 w26server
│   ├── run_mirror1.sh        # 单独启动 mirror1
│   ├── run_mirror2.sh        # 单独启动 mirror2
│   └── run_client.sh         # 启动客户端
├── doc/
│   ├── Project_W26.pdf       # 项目需求文档
│   ├── Requirement_Summary.md
│   └── Requirement_Summary_zh.md
├── Makefile
├── .gitignore
├── logs/                     # 服务端日志输出
├── .pids/                    # 服务端 PID 文件
└── out/                      # 编译产物
```

## 8. 编译

```bash
make clean && make
```

编译产物输出到 `out/` 目录。

## 9. 运行

### 一键启动

```bash
./scripts/start_all_servers.sh --depth 6
```

可选参数：
- `--root <path>`：指定文件搜索根目录（覆盖 `W26_SEARCH_ROOT`）
- `--depth <1-64>`：限制递归扫描深度（覆盖 `W26_MAX_SCAN_DEPTH`）

### 启动客户端

```bash
./out/client
```

连接后自动显示归属节点：

```text
client connected to w26server (127.0.0.1:5001), NODE: mirror1
```

### 查看状态 / 停止

```bash
./scripts/server_status.sh       # 查看进程状态
./scripts/stop_all_servers.sh    # 停止所有服务端
```

## 10. 支持的命令

| 命令 | 说明 | 响应类型 |
|---|---|---|
| `dirlist -a` | 按名称排序列出子目录 | 文本行 |
| `dirlist -t` | 按时间排序列出子目录 | 文本行 |
| `fn <filename>` | 查找文件并返回元数据（名称/大小/时间/权限） | 文本行 |
| `fz <size1> <size2>` | 按文件大小范围筛选，打包回传 | `FILE <size>` + 二进制 |
| `ft <ext1> [ext2] [ext3]` | 按扩展名筛选，打包回传（最多 3 个） | `FILE <size>` + 二进制 |
| `fdb <YYYY-MM-DD>` | 筛选**早于**指定日期的文件，打包回传 | `FILE <size>` + 二进制 |
| `fda <YYYY-MM-DD>` | 筛选**晚于等于**指定日期的文件，打包回传 | `FILE <size>` + 二进制 |
| `quitc` | 断开连接 | `BYE` |

压缩包文件保存到客户端的 `~/project/temp.tar.gz`。

## 11. 环境变量

| 变量 | 默认值 | 说明 |
|---|---|---|
| `W26_SEARCH_ROOT` | `$HOME` | 文件搜索根目录 |
| `W26_MAX_SCAN_DEPTH` | `8` | 递归扫描最大深度（1-64） |

在大目录下执行文件检索时，建议限制搜索范围以避免耗时过长：

```bash
./scripts/start_all_servers.sh --root ~/workspace --depth 4
```

## 12. 关键实现细节

### 序号原子性

`w26server` 采用 `fork-per-connection` 模型，父进程的内存变量无法被子进程回写。因此客户端序号通过 `/tmp/w26_client_seq.txt` 文件持久化，使用 `fcntl` 文件锁保证跨子进程的原子递增。

### 心跳与序号隔离

mirror 节点每 2 秒向 w26server 发送一次 HEARTBEAT 短连接。`crequest()` 通过读取第一条命令来区分心跳和客户端——心跳连接处理后立即退出，**永远不会触发 `next_client_seq()`**，从而不干扰客户端的序号分配。

### 进程回收

主进程通过 `signal(SIGCHLD, SIG_IGN)` 忽略子进程退出信号，由内核自动回收僵尸进程，避免 `waitpid` 阻塞主循环。

### stale PID 自动清理

`start_all_servers.sh` 在启动前检测 `.pids/` 下的 PID 文件：如果对应进程仍在运行则报错，如果进程已死则自动清理残留 PID 文件后正常启动。
