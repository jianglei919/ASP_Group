# ASP_Group Project README

## 1. 项目简介
本项目是一个基于 Socket 的客户端-服务端文件检索系统，包含 1 个主服务端和 2 个镜像服务端：

- w26server（主服务）
- mirror1（镜像服务 1）
- mirror2（镜像服务 2）
- client（客户端）

当前代码为作业提交版骨架，核心业务逻辑保留了 TODO 标注，便于后续逐步实现。

## 2. 项目结构图
```text
ASP_Group/
├── src/
│   ├── w26server.c
│   ├── mirror1.c
│   ├── mirror2.c
│   └── client.c
├── Makefile
├── .gitignore
├── doc/
│   ├── Project_W26.pdf
│   ├── Requirement_Summary.md
│   └── Requirement_Summary_zh.md
└── out/
    ├── w26server
    ├── mirror1
    ├── mirror2
    └── client
```

## 3. 架构图
```mermaid
flowchart LR
    U[用户] --> CL[client]
    CL -->|Socket 请求| S1[w26server]
    CL -->|Socket 请求| S2[mirror1]
    CL -->|Socket 请求| S3[mirror2]

    S1 --> FS1[服务端文件系统检索]
    S2 --> FS2[服务端文件系统检索]
    S3 --> FS3[服务端文件系统检索]

    FS1 -->|文本结果 或 temp.tar.gz| CL
    FS2 -->|文本结果 或 temp.tar.gz| CL
    FS3 -->|文本结果 或 temp.tar.gz| CL

    CL -->|保存压缩包到 ~/project| P[客户端 project 目录]

    classDef user fill:#FFE08A,stroke:#B7791F,stroke-width:2px,color:#2D1B00;
    classDef client fill:#9AE6B4,stroke:#2F855A,stroke-width:2px,color:#0F2E1D;
    classDef server fill:#90CDF4,stroke:#2B6CB0,stroke-width:2px,color:#102A43;
    classDef fs fill:#FBD38D,stroke:#B7791F,stroke-width:2px,color:#4A2C00;
    classDef storage fill:#E9D8FD,stroke:#6B46C1,stroke-width:2px,color:#2D1B69;

    class U user;
    class CL client;
    class S1,S2,S3 server;
    class FS1,FS2,FS3 fs;
    class P storage;

    linkStyle 0 stroke:#2F855A,stroke-width:2px;
    linkStyle 1 stroke:#2B6CB0,stroke-width:2px;
    linkStyle 2 stroke:#2B6CB0,stroke-width:2px;
    linkStyle 3 stroke:#2B6CB0,stroke-width:2px;
    linkStyle 4 stroke:#B7791F,stroke-width:2px;
    linkStyle 5 stroke:#B7791F,stroke-width:2px;
    linkStyle 6 stroke:#B7791F,stroke-width:2px;
    linkStyle 7 stroke:#805AD5,stroke-width:2px;
    linkStyle 8 stroke:#805AD5,stroke-width:2px;
    linkStyle 9 stroke:#805AD5,stroke-width:2px;
    linkStyle 10 stroke:#6B46C1,stroke-width:2px,stroke-dasharray: 4 2;
```

## 4. 编译脚本
当前 Makefile 会将编译产物统一输出到 out 目录。

```bash
#!/usr/bin/env bash
set -e

cd "$(dirname "$0")"
make clean
make
```

等效命令：

```bash
make clean && make
```

## 5. 运行脚本
建议使用 4 个终端分别运行 3 个服务端和 1 个客户端。

### 5.1 启动服务端
终端 1：

```bash
#!/usr/bin/env bash
set -e

cd "$(dirname "$0")"
./out/w26server
```

终端 2：

```bash
#!/usr/bin/env bash
set -e

cd "$(dirname "$0")"
./out/mirror1
```

终端 3：

```bash
#!/usr/bin/env bash
set -e

cd "$(dirname "$0")"
./out/mirror2
```

### 5.2 启动客户端
终端 4：

```bash
#!/usr/bin/env bash
set -e

cd "$(dirname "$0")"
./out/client
```

## 6. 常用测试命令示例
客户端启动后可输入：

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

## 7. 说明
- out 目录用于存放编译产物。
- .gitignore 已配置忽略 out 目录。
- 当前为骨架代码，未实现部分均以 TODO 标注。