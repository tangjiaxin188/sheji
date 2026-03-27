# DDoS 防护设计

## 防护策略

### 核心需求

1. **同一 IP 一天只能注册 5 个账号**
2. **同一账号只能同时在 5 个 IP 保持登录状态**

### 分层限流架构

```
┌─────────────────────────────────────────────────────────┐
│                    请求到达                              │
└─────────────────────────────────────────────────────────┘
                          │
                          ▼
┌─────────────────────────────────────────────────────────┐
│  第一层：IP 注册限流                                      │
│  - 单 IP 每日注册上限：5 个账号                            │
│  - 实现：SQLite 存储 IP + 日期 + 计数                     │
└─────────────────────────────────────────────────────────┘
                          │ 通过
                          ▼
┌─────────────────────────────────────────────────────────┐
│  第二层：账号登录 IP 限流                                  │
│  - 单账号最大登录 IP 数：5 个                               │
│  - 实现：数据库存储账号 -IP 映射                           │
└─────────────────────────────────────────────────────────┘
                          │ 通过
                          ▼
┌─────────────────────────────────────────────────────────┐
│  第三层：Token 速率限制 (已认证用户)                       │
│  - 单用户请求频率：50 次/秒                               │
│  - 实现：令牌桶算法 (内存)                                │
└─────────────────────────────────────────────────────────┘
```

## 数据库表设计

### ip_registrations 表 - IP 注册计数

```sql
CREATE TABLE ip_registrations (
    ip_address TEXT PRIMARY KEY,        -- IP 地址
    date TEXT NOT NULL,                 -- 日期 (YYYY-MM-DD)
    count INTEGER NOT NULL DEFAULT 0,   -- 注册次数
    last_reset INTEGER NOT NULL         -- 上次重置时间戳
);

CREATE INDEX idx_ip_reg_date ON ip_registrations(date);
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `ip_address` | TEXT | 客户端 IP 地址 |
| `date` | TEXT | 日期字符串 |
| `count` | INTEGER | 当日注册次数 |
| `last_reset` | INTEGER | 重置时间戳 |

### account_logins 表 - 账号登录 IP 映射

```sql
CREATE TABLE account_logins (
    client_id INTEGER NOT NULL,         -- 账号 ID
    ip_address TEXT NOT NULL,           -- 登录 IP
    chat_token TEXT NOT NULL,           -- 会话 Token
    login_time INTEGER NOT NULL,        -- 登录时间
    last_heartbeat INTEGER NOT NULL,    -- 心跳时间
    PRIMARY KEY (client_id, ip_address, chat_token)
);

CREATE INDEX idx_account_logins_client ON account_logins(client_id);
CREATE INDEX idx_account_logins_heartbeat ON account_logins(last_heartbeat);
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `client_id` | INTEGER | 账号 ID |
| `ip_address` | TEXT | 登录 IP |
| `chat_token` | TEXT | 会话 Token |
| `login_time` | INTEGER | 登录时间戳 |
| `last_heartbeat` | INTEGER | 心跳时间戳 |

## 核心实现

### 1. IP 注册限流 (第一层)

```c
// 检查 IP 当日注册次数
int check_ip_registration_limit(const char *ip) {
    pthread_mutex_lock(&g_state.db_lock);
    
    time_t now = time(NULL);
    char today[32];
    strftime(today, sizeof(today), "%Y-%m-%d", localtime(&now));
    
    const char *sql = 
        "INSERT OR REPLACE INTO ip_registrations (ip_address, date, count, last_reset) "
        "VALUES (?, ?, COALESCE((SELECT count FROM ip_registrations WHERE ip_address = ? AND date = ?), 0) + 1, ?)";
    
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(g_state.db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        pthread_mutex_unlock(&g_state.db_lock);
        return 0;
    }
    
    sqlite3_bind_text(stmt, 1, ip, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, today, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, ip, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, today, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 5, now);
    
    rc = sqlite3_step(stmt);
    
    // 检查是否超过限制
    if (rc == SQLITE_DONE) {
        // 查询当前计数
        const char *query = "SELECT count FROM ip_registrations WHERE ip_address = ? AND date = ?";
        sqlite3_stmt *query_stmt;
        if (sqlite3_prepare_v2(g_state.db, query, -1, &query_stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_text(query_stmt, 1, ip, -1, SQLITE_STATIC);
            sqlite3_bind_text(query_stmt, 2, today, -1, SQLITE_STATIC);
            
            if (sqlite3_step(query_stmt) == SQLITE_ROW) {
                int count = sqlite3_column_int(query_stmt, 0);
                sqlite3_finalize(query_stmt);
                
                if (count > 5) {
                    // 超过限制，回滚计数
                    sqlite3_exec(g_state.db, 
                        "UPDATE ip_registrations SET count = count - 1 WHERE ip_address = ? AND date = ?",
                        NULL, NULL, NULL);
                    pthread_mutex_unlock(&g_state.db_lock);
                    return 0;  // 限流
                }
                pthread_mutex_unlock(&g_state.db_lock);
                return 1;  // 允许
            }
            sqlite3_finalize(query_stmt);
        }
    }
    
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_state.db_lock);
    return 1;  // 默认允许
}

// 记录注册成功（认证成功后调用）
void record_registration_success(const char *ip) {
    // check_ip_registration_limit 已经增加了计数，无需额外操作
}

// 每日清理过期数据
void cleanup_ip_registrations() {
    pthread_mutex_lock(&g_state.db_lock);
    
    char yesterday[32];
    time_t now = time(NULL);
    strftime(yesterday, sizeof(yesterday), "%Y-%m-%d", localtime(&now));
    yesterday[10] = '\0';
    
    // 删除昨天的记录
    sqlite3_exec(g_state.db, 
        "DELETE FROM ip_registrations WHERE date < ?",
        NULL, NULL, NULL);
    
    pthread_mutex_unlock(&g_state.db_lock);
}
```

### 2. 账号登录 IP 限流 (第二层)

```c
// 检查账号登录 IP 数量
int check_account_login_limit(int client_id, const char *ip, const char *token) {
    pthread_mutex_lock(&g_state.db_lock);
    
    // 查询当前账号已登录的不同 IP 数量
    const char *count_sql = 
        "SELECT COUNT(DISTINCT ip_address) FROM account_logins WHERE client_id = ?";
    
    sqlite3_stmt *stmt;
    int ip_count = 0;
    
    if (sqlite3_prepare_v2(g_state.db, count_sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, client_id);
        
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            ip_count = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    
    // 检查是否已存在该 IP 的登录记录
    const char *exists_sql = 
        "SELECT chat_token FROM account_logins WHERE client_id = ? AND ip_address = ?";
    
    int exists = 0;
    char old_token[64] = {0};
    
    if (sqlite3_prepare_v2(g_state.db, exists_sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, client_id);
        sqlite3_bind_text(stmt, 2, ip, -1, SQLITE_STATIC);
        
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            exists = 1;
            const char *t = (const char *)sqlite3_column_text(stmt, 0);
            if (t) strncpy(old_token, t, sizeof(old_token) - 1);
        }
        sqlite3_finalize(stmt);
    }
    
    if (exists) {
        // 同一 IP 重新登录，更新旧 Token 的心跳
        sqlite3_exec(g_state.db,
            "UPDATE account_logins SET last_heartbeat = strftime('%s', 'now'), chat_token = ? "
            "WHERE client_id = ? AND ip_address = ?",
            NULL, NULL, NULL);
        
        // 删除旧 Token 记录
        sqlite3_stmt *del_stmt;
        if (sqlite3_prepare_v2(g_state.db, 
            "DELETE FROM account_logins WHERE client_id = ? AND ip_address = ? AND chat_token = ?",
            -1, &del_stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_int(del_stmt, 1, client_id);
            sqlite3_bind_text(del_stmt, 2, ip, -1, SQLITE_STATIC);
            sqlite3_bind_text(del_stmt, 3, old_token, -1, SQLITE_STATIC);
            sqlite3_step(del_stmt);
            sqlite3_finalize(del_stmt);
        }
    } else if (ip_count >= 5) {
        // 新 IP 且已达上限，清理最早的心跳超时记录
        sqlite3_exec(g_state.db,
            "DELETE FROM account_logins WHERE client_id = ? AND last_heartbeat = ("
            "  SELECT MIN(last_heartbeat) FROM account_logins WHERE client_id = ?"
            ")",
            NULL, NULL, NULL);
    }
    
    // 插入新登录记录
    const char *insert_sql = 
        "INSERT INTO account_logins (client_id, ip_address, chat_token, login_time, last_heartbeat) "
        "VALUES (?, ?, ?, strftime('%s', 'now'), strftime('%s', 'now'))";
    
    rc = sqlite3_prepare_v2(g_state.db, insert_sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, client_id);
        sqlite3_bind_text(stmt, 2, ip, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3, token, -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    
    pthread_mutex_unlock(&g_state.db_lock);
    return 1;  // 允许
}

// 移除登录记录（登出或超时）
void remove_account_login(int client_id, const char *token) {
    pthread_mutex_lock(&g_state.db_lock);
    
    sqlite3_stmt *stmt;
    const char *sql = "DELETE FROM account_logins WHERE client_id = ? AND chat_token = ?";
    
    if (sqlite3_prepare_v2(g_state.db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, client_id);
        sqlite3_bind_text(stmt, 2, token, -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    
    pthread_mutex_unlock(&g_state.db_lock);
}

// 清理超时登录记录
void cleanup_timeout_logins(int timeout_seconds) {
    pthread_mutex_lock(&g_state.db_lock);
    
    const char *sql = "DELETE FROM account_logins WHERE last_heartbeat < strftime('%s', 'now') - ?";
    sqlite3_stmt *stmt;
    
    if (sqlite3_prepare_v2(g_state.db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, timeout_seconds);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    
    pthread_mutex_unlock(&g_state.db_lock);
}
```

### 3. Token 令牌桶限流 (第三层)

```c
// 令牌桶结构
typedef struct {
    int tokens;          // 当前令牌数
    int max_tokens;      // 最大容量
    time_t last_refill;  // 上次补充时间
    int refill_rate;     // 每秒补充数
} token_bucket_t;

token_bucket_t user_buckets[HASH_BUCKET_SIZE];

int check_token_bucket(const char *token) {
    uint32_t idx = hash_token(token, TOKEN_LEN);
    token_bucket_t *bucket = &user_buckets[idx];
    time_t now = time(NULL);
    
    // 初始化
    if (bucket->max_tokens == 0) {
        bucket->max_tokens = TOKEN_BUCKET_MAX;
        bucket->tokens = TOKEN_BUCKET_MAX;
        bucket->refill_rate = TOKEN_BUCKET_RATE;
        bucket->last_refill = now;
    }
    
    // 补充令牌
    int elapsed = now - bucket->last_refill;
    if (elapsed > 0) {
        bucket->tokens += elapsed * bucket->refill_rate;
        if (bucket->tokens > bucket->max_tokens) {
            bucket->tokens = bucket->max_tokens;
        }
        bucket->last_refill = now;
    }
    
    // 消耗令牌
    if (bucket->tokens > 0) {
        bucket->tokens--;
        return 1;  // 允许
    }
    
    return 0;  // 限流
}
```

## 集成到认证流程

### 注册/认证请求处理

```c
void handle_auth_request(struct mg_connection *c, const char *data_str) {
    // 获取客户端 IP
    char ip[64];
    snprintf(ip, sizeof(ip), "%d.%d.%d.%d",
             (c->rem.ip >> 0) & 0xFF,
             (c->rem.ip >> 8) & 0xFF,
             (c->rem.ip >> 16) & 0xFF,
             (c->rem.ip >> 24) & 0xFF);
    
    // 第一层：IP 注册限流
    if (!check_ip_registration_limit(ip)) {
        send_error(c, 100, "该 IP 今日注册次数已达上限 (5 次/天)", "AUTH_REQUEST");
        return;
    }
    
    // ... 正常认证流程
}

void handle_auth_response(struct mg_connection *c, const char *data_str) {
    // ... 验证签名 ...
    
    // 认证成功，生成 Token
    generate_chat_token(session->client_id, session->chat_token);
    
    // 获取客户端 IP
    char ip[64];
    snprintf(ip, sizeof(ip), "%d.%d.%d.%d",
             (c->rem.ip >> 0) & 0xFF,
             (c->rem.ip >> 8) & 0xFF,
             (c->rem.ip >> 16) & 0xFF,
             (c->rem.ip >> 24) & 0xFF);
    
    // 第二层：账号登录 IP 限流
    check_account_login_limit(session->client_id, ip, session->chat_token);
    
    // 绑定 Token
    bind_token_to_session(session, session->chat_token);
    
    // ... 返回成功响应
}
```

### 需要认证的请求处理

```c
void handle_chat_send(struct mg_connection *c, const char *data_str) {
    struct mg_str data = parse_data_json(data_str, strlen(data_str));
    char *token = mg_json_get_str(data, "$.chat_token");
    
    // 查找会话
    client_session_t *session = token ? find_session_by_token(token) : NULL;
    if (!session || session->state != CLIENT_STATE_AUTHED) {
        send_error(c, 10, "未认证", "CHAT_SEND");
        if (token) free(token);
        return;
    }
    
    // 第三层：Token 限流
    if (!check_token_bucket(token)) {
        send_error(c, 103, "操作过于频繁", "CHAT_SEND");
        free(token);
        return;
    }
    
    // 更新心跳
    session->last_heartbeat = time(NULL);
    session->conn = c;
    
    // ... 继续处理
}
```

## 定时清理任务

```c
void periodic_cleanup(struct mg_mgr *mgr) {
    (void)mgr;
    
    // 每 30 秒清理超时登录
    cleanup_timeout_logins(g_config.heartbeat_timeout);
    
    // 每天凌晨清理过期 IP 注册记录
    static time_t last_cleanup_date = 0;
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    
    if (tm_info->tm_hour == 0 && tm_info->tm_min == 0 && last_cleanup_date != now / 86400) {
        cleanup_ip_registrations();
        last_cleanup_date = now / 86400;
    }
}
```

## 防护效果

| 攻击类型 | 防护层级 | 限制 |
|----------|---------|------|
| 单 IP 批量注册 | 第一层 | 5 个账号/天 |
| 单账号多 IP 登录 | 第二层 | 5 个 IP 同时在线 |
| Token 滥用 | 第三层 | 50 次/秒 |

## 内存占用

| 数据结构 | 内存占用 |
|----------|---------|
| IP 注册表 | SQLite 存储 |
| 账号登录表 | SQLite 存储 |
| Token 令牌桶 | ~256 KB |
| **总计** | **< 1 MB** |
