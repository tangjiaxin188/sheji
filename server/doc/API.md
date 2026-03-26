# 服务器 API 接口文档

## 概述

本文档描述了聊天服务器的 HTTP API 接口。所有接口均基于 HTTP/1.1 协议，使用 JSON 格式进行数据交换。

**服务器地址**: `http://<server_ip>:25565`

**通用请求头**:
```
Content-Type: application/json
```

---

## 数据格式

### 请求格式

所有请求均使用 `POST` 方法发送到 `/api` 端点，请求体格式如下：

```json
{
  "COMMAND": "命令名称",
  "data": "JSON 字符串（包含具体数据）"
}
```

> **注意**: `data` 字段是一个 **JSON 字符串**，需要二次解析。

### 响应格式

成功响应：
```json
{
  "COMMAND": "响应命令",
  "data": { ... }
}
```

错误响应：
```json
{
  "COMMAND": "ERROR",
  "data": {
    "error_code": 错误码,
    "error_msg": "错误信息",
    "command": "失败的命令"
  }
}
```

---

## 接口列表

### 1. 注册流程

#### 1.1 注册请求 (SIGNUP_ACCEPT)

客户端发送公钥到服务器进行注册。

**请求**:
```bash
POST /api
{
  "COMMAND": "SIGNUP_ACCEPT",
  "data": "{\"pub_key\":\"客户端公钥\"}"
}
```

**成功响应**:
```json
{
  "COMMAND": "SIGNUP_CHECK",
  "data": {
    "check_data": "随机挑战数据"
  }
}
```

**错误码**:
| 错误码 | 说明 |
|--------|------|
| 1 | 客户端数量已达上限 |
| 2 | 缺少公钥 |

---

#### 1.2 注册验证 (SIGNUP_CHECK)

客户端用私钥签名挑战数据后发送给服务器。

**请求**:
```bash
POST /api
{
  "COMMAND": "SIGNUP_CHECK",
  "data": "{\"check_data\":\"签名后的挑战数据\"}"
}
```

**成功响应**:
```json
{
  "COMMAND": "SIGNUP_AGREE",
  "data": {
    "id": 用户 ID
  }
}
```

**错误码**:
| 错误码 | 说明 |
|--------|------|
| 3 | 注册流程错误 |
| 4 | 缺少签名数据 |

---

### 2. 认证流程

#### 2.1 认证请求 (AUTH_REQUEST)

已注册用户发起认证请求。

**请求**:
```bash
POST /api
{
  "COMMAND": "AUTH_REQUEST",
  "data": "{\"client_id\":\"用户 ID\"}"
}
```

**成功响应**:
```json
{
  "COMMAND": "AUTH_CHALLENGE",
  "data": {
    "challenge_data": "随机挑战数据"
  }
}
```

**错误码**:
| 错误码 | 说明 |
|--------|------|
| 1 | 客户端数量已达上限 |
| 5 | 缺少客户端 ID |

---

#### 2.2 认证响应 (AUTH_RESPONSE)

客户端签名挑战数据后发送。

**请求**:
```bash
POST /api
{
  "COMMAND": "AUTH_RESPONSE",
  "data": "{\"signature\":\"签名数据\"}"
}
```

**成功响应**:
```json
{
  "COMMAND": "AUTH_SUCCESS",
  "data": {
    "chat_token": "聊天令牌",
    "contacts": [
      {"id": 1, "name": "user1", "online": true},
      {"id": 2, "name": "user2", "online": false}
    ]
  }
}
```

**错误码**:
| 错误码 | 说明 |
|--------|------|
| 6 | 认证流程错误 |
| 7 | 缺少签名 |

---

### 3. 心跳保活

#### 3.1 心跳请求 (HEARTBEAT_REQUEST)

客户端定时发送心跳保持连接活跃。

**请求**:
```bash
POST /api
{
  "COMMAND": "HEARTBEAT_REQUEST",
  "data": "{\"timestamp\":1234567890}"
}
```

**成功响应**:
```json
{
  "COMMAND": "HEARTBEAT_RESPONSE",
  "data": {
    "timestamp": 1234567890,
    "contacts_update": []
  }
}
```

**错误码**:
| 错误码 | 说明 |
|--------|------|
| 8 | 未认证 |

---

### 4. 通讯录管理

#### 4.1 通讯录请求 (CONTACTS_REQUEST)

客户端请求获取通讯录列表。

**请求**:
```bash
POST /api
{
  "COMMAND": "CONTACTS_REQUEST",
  "data": "{}"
}
```

**成功响应**:
```json
{
  "COMMAND": "CONTACTS_RESPONSE",
  "data": {
    "contacts": [
      {"id": 1, "name": "user1", "online": true},
      {"id": 2, "name": "user2", "online": false}
    ]
  }
}
```

**错误码**:
| 错误码 | 说明 |
|--------|------|
| 9 | 未认证 |

---

### 5. 消息发送

#### 5.1 发送消息 (CHAT_SEND)

客户端发送聊天消息或文件。

**请求**:
```bash
POST /api
{
  "COMMAND": "CHAT_SEND",
  "data": "{\"target_id\":123,\"chat_token\":\"令牌\",\"content_type\":\"text/bin\",\"content\":\"base64 编码的内容\"}"
}
```

**参数说明**:
| 参数 | 类型 | 说明 |
|------|------|------|
| target_id | int | 目标用户 ID |
| chat_token | string | 聊天令牌 |
| content_type | string | 内容类型：`text`=文本，`bin`=二进制 |
| content | string | base64 编码的内容 |

**成功响应**:
```json
{
  "COMMAND": "SEND_FILE_ACK",
  "data": {
    "file_id": "file_xxx",
    "status": "success",
    "msg": ""
  }
}
```

**错误码**:
| 错误码 | 说明 |
|--------|------|
| 10 | 未认证 |
| 11 | 缺少必要字段 |

---

### 6. 文件传输

#### 6.1 开始文件传输 (FILE_TRANSFER_START)

通知服务器准备接收文件。

**请求**:
```bash
POST /api
{
  "COMMAND": "FILE_TRANSFER_START",
  "data": "{\"file_id\":\"xxx\",\"file_name\":\"文件名.txt\",\"file_size\":123456,\"content_type\":\"bin\"}"
}
```

**参数说明**:
| 参数 | 类型 | 说明 |
|------|------|------|
| file_id | string | 文件唯一标识 |
| file_name | string | 文件名 |
| file_size | int | 文件总大小（字节） |
| content_type | string | 内容类型 |

**成功响应**:
```json
{
  "COMMAND": "FILE_TRANSFER_ACK",
  "data": {
    "file_id": "xxx",
    "status": "ready",
    "upload_url": "/upload/xxx"
  }
}
```

**错误码**:
| 错误码 | 说明 |
|--------|------|
| 10 | 未认证 |
| 14 | 缺少必要字段 |
| 15 | 文件传输会话已达上限 |
| 16 | 内存分配失败 |

---

#### 6.2 发送文件块 (FILE_TRANSFER_CHUNK)

发送文件数据块（兼容模式，推荐直接使用 PUT /upload）。

**请求**:
```bash
POST /api
{
  "COMMAND": "FILE_TRANSFER_CHUNK",
  "data": "{\"file_id\":\"xxx\",\"chunk_index\":0,\"content\":\"base64 编码的块数据\"}"
}
```

**成功响应**:
```json
{
  "COMMAND": "FILE_TRANSFER_CHUNK_ACK",
  "data": {
    "file_id": "xxx",
    "chunk_index": 0,
    "status": "received",
    "next_chunk": 1
  }
}
```

**错误码**:
| 错误码 | 说明 |
|--------|------|
| 10 | 未认证 |
| 14 | 缺少必要字段 |
| 17 | 文件传输会话不存在 |
| 18 | 无效的块索引 |

---

#### 6.3 结束文件传输 (FILE_TRANSFER_END)

通知服务器文件传输完成。

**请求**:
```bash
POST /api
{
  "COMMAND": "FILE_TRANSFER_END",
  "data": "{\"file_id\":\"xxx\",\"checksum\":\"sha256 校验和\"}"
}
```

**成功响应**:
```json
{
  "COMMAND": "FILE_TRANSFER_COMPLETE",
  "data": {
    "file_id": "xxx",
    "status": "success",
    "msg": "文件接收完成"
  }
}
```

**错误码**:
| 错误码 | 说明 |
|--------|------|
| 10 | 未认证 |
| 14 | 缺少 file_id |
| 17 | 文件传输会话不存在 |
| 19 | 缺少数据块 |

---

### 7. 文件上传端点

#### 7.1 Multipart 上传

**请求**:
```bash
POST /upload
Content-Type: multipart/form-data; boundary=---BOUNDARY

-----BOUNDARY
Content-Disposition: form-data; name="file"; filename="test.txt"
Content-Type: application/octet-stream

<二进制数据>
-----BOUNDARY--
```

**成功响应**:
```json
{
  "COMMAND": "UPLOAD_COMPLETE",
  "data": {
    "file_id": "file_xxx",
    "status": "success",
    "msg": "文件上传成功"
  }
}
```

---

#### 7.2 分块上传 (PUT)

**请求**:
```bash
PUT /upload/xxx?chunk=0
Content-Type: application/octet-stream

<二进制块数据>
```

**成功响应**:
```json
{
  "COMMAND": "FILE_TRANSFER_CHUNK_ACK",
  "data": {
    "chunk_index": 0,
    "status": "received",
    "next_chunk": 1
  }
}
```

---

## 通用错误响应

所有错误通过 HTTP 状态码 + JSON 响应体返回：

**HTTP 状态码**:
| 状态码 | 说明 |
|--------|------|
| 200 | 请求成功 |
| 400 | 客户端错误（JSON 格式错误、缺少字段等） |
| 401 | 未认证或认证失败 |
| 404 | 端点不存在 |
| 405 | 方法不允许 |
| 500 | 服务器内部错误 |

**错误响应格式**:
```json
{
  "COMMAND": "ERROR",
  "data": {
    "error_code": 12,
    "error_msg": "无效的 JSON 格式",
    "command": "UNKNOWN"
  }
}
```

---

## 使用示例

### cURL 示例

```bash
# 注册
curl -X POST http://localhost:25565/api \
  -H "Content-Type: application/json" \
  -d '{"COMMAND":"SIGNUP_ACCEPT","data":"{\"pub_key\":\"my_pub_key\"}"}'

# 心跳
curl -X POST http://localhost:25565/api \
  -H "Content-Type: application/json" \
  -d '{"COMMAND":"HEARTBEAT_REQUEST","data":"{\"timestamp\":1234567890}"}'

# 获取通讯录
curl -X POST http://localhost:25565/api \
  -H "Content-Type: application/json" \
  -d '{"COMMAND":"CONTACTS_REQUEST","data":"{}"}'
```

### Python 示例

```python
import requests
import json

BASE_URL = "http://localhost:25565/api"

def signup(pub_key):
    response = requests.post(BASE_URL, json={
        "COMMAND": "SIGNUP_ACCEPT",
        "data": json.dumps({"pub_key": pub_key})
    })
    return response.json()

def heartbeat(timestamp):
    response = requests.post(BASE_URL, json={
        "COMMAND": "HEARTBEAT_REQUEST",
        "data": json.dumps({"timestamp": timestamp})
    })
    return response.json()

# 使用示例
result = signup("my_public_key")
print(result)
```

---

## 会话管理

> **注意**: 当前实现为测试目的，会话通过连接或状态识别。实际生产环境中应通过 `chat_token` 或其他认证机制识别用户。

建议在认证成功后，在后续请求中携带 `chat_token` 进行身份验证。
