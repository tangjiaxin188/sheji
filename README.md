# 去中心化加密聊天系统

## 设计理念

服务器不存储具体聊天信息，仅作为中转站转发加密数据，保障去中心化的聊天安全。所有身份验证基于 **Ed25519 非对称加密**，客户端私钥永不传输，服务端只存储公钥。信息传递基于 HTTP 协议和 JSON 格式，保证高可用和复用性，也可使用 HTTPS 协议提高安全性。

### 安全特性

- **私钥本地生成**：客户端本地生成 Ed25519 密钥对，私钥永不离开客户端
- **挑战 - 响应认证**：每次登录服务端发送随机挑战数据，客户端用私钥签名，服务端用公钥验证
- **Token 会话机制**：认证成功后生成会话 Token，后续请求凭 Token 识别身份
- **DDoS 防护**：
  - 同一 IP 每日最多注册 5 个账号
  - 同一账号最多同时在 5 个 IP 保持登录状态
  - Token 请求限流：50 次/秒

## 依赖库

| 组件 | 库 | 用途 |
|------|-----|------|
| **服务端** | mongoose | HTTP/WebSocket 网络库 |
| **服务端** | Monocypher | Ed25519 加密签名库 |
| **服务端** | MiniZ | 数据压缩/解压库 |
| **服务端** | SQLite | 用户数据和会话存储 |
| **客户端** | raylib | UI 界面渲染库 |

## 具体流程

### 注册/认证流程

```
┌─────────────┐                          ┌─────────────┐
│   客户端     │                          │    服务端    │
└──────┬──────┘                          └──────┬──────┘
       │                                        │
       │  1. 生成密钥对 (本地)                   │
       │     私钥 [64], 公钥 [32]                │
       │                                        │
       │  ───── AUTH_REQUEST + 公钥 ──────────► │ 检查 IP 注册限流 (5 次/天)
       │                                        │ 存储公钥到数据库
       │                                        │ 生成挑战数据 challenge[32]
       │                                        │
       │  ◄ ──── AUTH_CHALLENGE + challenge ─── │
       │                                        │
       │  2. 签名：sig = sign(私钥，challenge)   │
       │                                        │
       │  ───── AUTH_RESPONSE + sig ──────────► │ 验证签名：check(sig, 公钥，challenge)
       │                                        │ ✓ → 生成 chat_token
       │                                        │ 记录账号 -IP 映射 (最多 5 个 IP)
       │  ◄ ──── AUTH_SUCCESS + chat_token ──── │
       │                                        │
       │  3. 后续请求携带 chat_token ──────────► │ 验证 Token + 限流 (50 次/秒)
```

### 消息发送流程

1. 客户端使用目标用户公钥加密消息内容
2. 文本直接加密，文件先压缩 (MiniZ) 再加密
3. 发送请求携带 `chat_token`、目标 ID、加密内容
4. 服务端验证 Token 有效性，转发加密数据给目标用户
5. 目标用户用私钥解密获取原始内容

## 客户端数据处理

| 数据类型 | 处理方式 |
|----------|---------|
| 文本消息 | Ed25519 签名 + 目标公钥加密 |
| 文件消息 | MiniZ 压缩 → Ed25519 签名 → 目标公钥加密 |
| 认证请求 | Ed25519 签名挑战数据 |

## 服务端内容存储

### 数据库表结构 (SQLite)

```sql
-- 用户公钥表
CREATE TABLE clients (
    client_id INTEGER PRIMARY KEY,
    public_key BLOB NOT NULL        -- 32 字节 Ed25519 公钥
);

-- IP 注册限流表
CREATE TABLE ip_registrations (
    ip_address TEXT PRIMARY KEY,
    date TEXT NOT NULL,             -- YYYY-MM-DD
    count INTEGER NOT NULL          -- 当日注册次数
);

-- 账号登录 IP 映射表
CREATE TABLE account_logins (
    client_id INTEGER NOT NULL,
    ip_address TEXT NOT NULL,
    chat_token TEXT NOT NULL,
    last_heartbeat INTEGER NOT NULL
);

-- 挑战数据表 (临时)
CREATE TABLE challenges (
    client_id INTEGER PRIMARY KEY,
    challenge BLOB NOT NULL,        -- 32 字节随机挑战
    expires_at INTEGER NOT NULL     -- 5 分钟过期
);
```

### 存储策略

- **不存储聊天内容**：仅转发，不落地
- **不存储私钥**：客户端完全控制身份
- **不存储密码**：使用非对称加密，无需密码

## 编译方法

本项目使用 CMake 构建，支持跨平台编译。

### 服务端编译

```bash
cd server/build
cmake ..
make -j4
./mongoose_demo
```

### 客户端编译

```bash
cd client/build
cmake ..
make -j4
./client_demo
```

### 依赖安装

```bash
# Ubuntu/Debian
sudo apt-get install libsqlite3-dev

# macOS
brew install sqlite

# Windows (vcpkg)
vcpkg install sqlite3
```

## 配置说明

服务端配置文件 `config.json`：

```json
{
    "port": 25565,
    "log_level": 4,
    "max_clients": 100000,
    "heartbeat_timeout": 120,
    "db_path": "./server.db"
}
```

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `port` | 监听端口 | 25565 |
| `log_level` | 日志级别 (0-4) | 4 |
| `max_clients` | 最大并发用户数 | 100000 |
| `heartbeat_timeout` | 心跳超时 (秒) | 120 |
| `db_path` | SQLite 数据库路径 | ./server.db |

## 项目结构

```
.
├── server/                 # 服务端代码
│   ├── src/
│   │   ├── lib.c          # 核心业务逻辑
│   │   ├── lib.h          # 头文件
│   │   └── main.c         # 入口
│   ├── include/           # 第三方库头文件
│   └── doc/               # 设计文档
│       ├── 认证架构设计.md
│       ├── 数据库设计.md
│       └── DDoS 防护设计.md
├── client/                # 客户端代码
│   ├── src/
│   ├── include/
│   └── UI/
├── miniz/                 # MiniZ 压缩库
├── mongoose/              # Mongoose 网络库
├── Monocypher/            # Monocypher 加密库
└── raylib/                # Raylib 图形库
```

## 文档

- [认证架构设计](server/doc/认证架构设计.md) - Ed25519 挑战 - 响应认证流程
- [数据库设计](server/doc/数据库设计.md) - SQLite 表结构和 API
- [DDoS 防护设计](server/doc/DDoS 防护设计.md) - 限流和防护策略
