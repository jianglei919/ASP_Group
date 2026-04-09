# ASP_Group Project README

## 1. 项目概述
本项目是一个基于 Socket 的分布式文件检索系统，由 1 个主服务端和 2 个镜像服务端组成：

- `w26server`：MainServer，同时也是一个真实业务处理节点。
- `mirror1`：镜像业务节点 1。
- `mirror2`：镜像业务节点 2。
- `client`：客户端。

当前实现支持文本检索、文件元数据查询、按条件打包并以二进制流回传。

## 2. 最终设计目标
最终设计采用“全局连接序号驱动的会话归属”模式，而不是“主服务端代理所有业务流量”模式。

设计重点如下：

- `w26server` 既负责接入与分流，也负责自己序号段内的业务处理。
- `mirror1` 和 `mirror2` 与 `w26server` 处于同一层级，都是实际业务处理节点。
- 每个连接一旦归属某个节点，就固定在该节点上处理完整会话。
- 分流依据必须是全局新连接序号，而不是请求数，也不是会话内局部变量。

## 3. 连接序号规则
连接序号的目标分配规则如下：

- 第 1-2 个连接由 `w26server` 处理。
- 第 3-4 个连接由 `mirror1` 处理。
- 第 5-6 个连接由 `mirror2` 处理。
- 从第 7 个连接开始按 `w26server -> mirror1 -> mirror2` 循环轮转。

因此，第 7、10、13、16 ... 个连接都应回到 `w26server`。

## 4. 连接建立流程
1. `client` 首次连接 `w26server`。
2. `w26server` 在 accept 新连接时分配全局连接序号。
3. `w26server` 根据序号判断该连接归属哪个节点。
4. 若归属是 `w26server`，则当前连接直接进入业务处理。
5. 若归属是 `mirror1` 或 `mirror2`，则返回重定向信息，让 `client` 重新连接目标节点。
6. `client` 重连后，后续命令都在目标节点上完成。

## 5. 节点职责
- `w26server`：负责前 1-2 个连接，同时也是所有连接的入口与全局序号分配点。
- `mirror1`：负责第 3-4 个连接，以及后续轮转中归属到自己的连接。
- `mirror2`：负责第 5-6 个连接，以及后续轮转中归属到自己的连接。

三个节点都必须具备完整的业务处理能力，不能只依赖主服务端代理转发。

## 6. 会话模型
- 一个连接只归属于一个节点。
- 同一会话中的后续命令保持在同一节点上处理。
- 不允许每条命令都重新做分流决策。
- 不允许在同一会话里反复在三个节点之间跳转。

## 7. 协议建议
推荐的会话协议如下：

- `HELLO`：`client` 首次建立会话时发起握手。
- `STAY`：表示当前连接由 `w26server` 直接处理。
- `REDIRECT host port`：表示该连接应重连到指定 `mirror`。
- `BYE`：表示会话结束。
- 业务命令仍沿用现有的 `dirlist`、`fn`、`fz`、`ft`、`fdb`、`fda`。

如果需要更强的约束，也可以给 `REDIRECT` 附加一次性 token，但作业场景下不是必需项。

## 8. 风险点
- `fork` 模型下，普通全局变量不能直接作为全局连接序号，必须保证进程间一致。
- `client` 的重连会带来一次额外握手，但能显著降低主服务端的数据面压力。
- 如果目标节点不可达，需要定义回退策略，否则会出现分配后无法接入的问题。
- 必须严格区分“连接序号”和“当前在线客户端数”，这两个概念不能混用。

## 9. 项目结构
```text
ASP_Group/
├── src/
│   ├── w26server.c
│   ├── mirror1.c
│   ├── mirror2.c
│   └── client.c
├── scripts/
│   ├── run_w26server.sh
│   ├── run_mirror1.sh
│   ├── run_mirror2.sh
│   ├── run_client.sh
│   ├── start_all_servers.sh
│   ├── stop_all_servers.sh
│   └── server_status.sh
├── Makefile
├── .gitignore
├── doc/
│   ├── Project_W26.pdf
│   ├── Requirement_Summary.md
│   └── Requirement_Summary_zh.md
├── logs/
├── .pids/
└── out/
```

## 10. 编译
统一执行：

```bash
make clean && make
```

编译产物输出到 `out/`。

## 11. 运行
### 11.1 逐个启动
```bash
./scripts/run_w26server.sh
./scripts/run_mirror1.sh
./scripts/run_mirror2.sh
./scripts/run_client.sh
```

### 11.2 一键启动服务端
```bash
./scripts/start_all_servers.sh
```

可指定扫描根目录与递归深度：

```bash
./scripts/start_all_servers.sh --root /path/to/search --depth 6
```

查看状态：

```bash
./scripts/server_status.sh
```

停止服务端：

```bash
./scripts/stop_all_servers.sh
```

## 12. 常用命令
```text
dirlist -a
dirlist -t
fn sample.txt
fz 100 10000
ft c txt
fdb 2026-01-01
fda 2026-03-31
quitc
```

## 13. 文件检索与归档说明
- `dirlist -a`：按名称排序列出目录。
- `dirlist -t`：按时间排序列出目录。
- `fn <filename>`：返回文件名、大小、创建时间和权限信息。
- `fz <size1> <size2>`：按文件大小范围筛选并回传压缩包。
- `ft <ext1> [ext2] [ext3]`：按扩展名筛选并回传压缩包。
- `fdb <YYYY-MM-DD>`：筛选早于指定日期的文件。
- `fda <YYYY-MM-DD>`：筛选晚于等于指定日期的文件。
- 命中 `fz/ft/fdb/fda` 时，服务端按 `FILE <size>\n + 二进制数据` 协议发送压缩包，客户端保存到 `~/project/temp.tar.gz`。

## 14. 环境变量
支持以下环境变量控制扫描范围与开销：

- `W26_SEARCH_ROOT`：覆盖默认搜索根目录，默认值为 `HOME`。
- `W26_MAX_SCAN_DEPTH`：限制递归深度，默认 `8`，取值范围 `1-64`。

在大目录执行 `fda/fdb` 时，建议设置较小的 `W26_SEARCH_ROOT` 和 `W26_MAX_SCAN_DEPTH`，避免耗时过长。