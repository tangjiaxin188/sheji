#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "mongoose.h"
#include "miniz.h"

// ==================== 数据结构定义 ====================

#define TOKEN_LEN 64
#define MAX_CHUNK_SIZE (64 * 1024)  // 64KB 每块

// 客户端连接状态
typedef enum {
    CLIENT_STATE_INIT,      // 初始连接
    CLIENT_STATE_SIGNUP,    // 注册中
    CLIENT_STATE_AUTHED,    // 已认证
} client_state_t;

// 联系人信息
typedef struct {
    int id;
    char name[64];
    int online;
} contact_t;

// 文件传输会话
typedef struct {
    char file_id[64];
    char file_name[256];
    uint32_t file_size;
    uint32_t chunk_size;
    int total_chunks;
    int received_chunks;
    char content_type[16];
    unsigned char *buffer;
    uint32_t buffer_size;
    time_t start_time;
    int active;
} file_transfer_t;

// 客户端会话
typedef struct {
    struct mg_connection *conn;
    client_state_t state;
    int client_id;
    char chat_token[TOKEN_LEN];
    contact_t *contacts;
    int contact_count;
    int contact_capacity;
    time_t last_heartbeat;
    file_transfer_t *pending_files;
    int pending_file_count;
    int pending_file_capacity;
} client_session_t;

// 配置结构
typedef struct {
    int port;
    int log_level;
    int max_clients;
    int heartbeat_timeout;
    int default_contact_capacity;
    int default_pending_file_capacity;
} server_config_t;

// 全局状态
static struct {
    client_session_t *clients;
    int next_client_id;
} g_state = {0};

// 默认配置
static server_config_t g_config = {
    .port = 25565,
    .log_level = 4,
    .max_clients = 100,
    .heartbeat_timeout = 120,
    .default_contact_capacity = 100,
    .default_pending_file_capacity = 10
};

// ==================== 函数声明 ====================

static client_session_t *find_client(struct mg_connection *c);
static client_session_t *create_client(struct mg_connection *c);
static void send_json_response(struct mg_connection *c, const char *command, const char *data);
static void send_error(struct mg_connection *c, int code, const char *msg, const char *failed_cmd);
static void generate_token(char *buf, size_t len);
static unsigned char *decompress_data(const unsigned char *input, size_t input_len, size_t *output_len);

static void handle_signup_accept(struct mg_connection *c, struct mg_str data);
static void handle_signup_check(struct mg_connection *c, struct mg_str data);
static void handle_auth_request(struct mg_connection *c, struct mg_str data);
static void handle_auth_response(struct mg_connection *c, struct mg_str data);
static void handle_heartbeat_request(struct mg_connection *c, struct mg_str data);
static void handle_contacts_request(struct mg_connection *c, struct mg_str data);
static void handle_chat_send(struct mg_connection *c, struct mg_str data);
static void handle_file_transfer_start(struct mg_connection *c, struct mg_str data);
static void handle_file_transfer_chunk(struct mg_connection *c, struct mg_str data);
static void handle_file_transfer_end(struct mg_connection *c, struct mg_str data);

static void fn(struct mg_connection *c, int ev, void *ev_data);
static void load_config(const char *config_path);
static void resize_contacts(client_session_t *session, int new_capacity);
static void resize_pending_files(client_session_t *session, int new_capacity);
static file_transfer_t *find_or_create_transfer(client_session_t *session, const char *file_id);
static file_transfer_t *find_transfer(client_session_t *session, const char *file_id);
static void free_transfer(file_transfer_t *ft);

// ==================== 主函数 ====================

int main(void) {
    struct mg_mgr mgr;
    struct mg_connection *c;
    char listen_addr[64];
    
    // 加载配置文件
    load_config("config.json");
    
    // 初始化随机数种子
    srand((unsigned int)time(NULL));
    
    // 分配客户端会话数组
    g_state.clients = calloc(g_config.max_clients, sizeof(client_session_t));
    if (!g_state.clients) {
        MG_ERROR(("内存分配失败"));
        return 1;
    }
    
    // 初始化 mongoose
    mg_mgr_init(&mgr);
    
    // 创建 TCP 监听器
    snprintf(listen_addr, sizeof(listen_addr), "tcp://0.0.0.0:%d", g_config.port);
    c = mg_listen(&mgr, listen_addr, fn, NULL);
    if (!c) {
        MG_ERROR(("无法创建监听器"));
        return 1;
    }
    
    MG_INFO(("服务器启动，监听端口 %d", g_config.port));
    
    // 主循环
    for (;;) {
        mg_mgr_poll(&mgr, 1000);
        
        // 检查心跳超时
        time_t now = time(NULL);
        for (int i = 0; i < g_config.max_clients; i++) {
            if (g_state.clients[i].conn && 
                g_state.clients[i].state == CLIENT_STATE_AUTHED) {
                if (now - g_state.clients[i].last_heartbeat > g_config.heartbeat_timeout) {
                    MG_INFO(("客户端 %d 心跳超时", g_state.clients[i].client_id));
                }
            }
        }
    }
    
    // 清理
    for (int i = 0; i < g_config.max_clients; i++) {
        if (g_state.clients[i].contacts) {
            free(g_state.clients[i].contacts);
        }
        if (g_state.clients[i].pending_files) {
            for (int j = 0; j < g_state.clients[i].pending_file_capacity; j++) {
                if (g_state.clients[i].pending_files[j].buffer) {
                    free(g_state.clients[i].pending_files[j].buffer);
                }
            }
            free(g_state.clients[i].pending_files);
        }
    }
    free(g_state.clients);
    mg_mgr_free(&mgr);
    return 0;
}

// ==================== 事件处理函数 ====================

static void fn(struct mg_connection *c, int ev, void *ev_data) {
    if (ev == MG_EV_OPEN) {
        c->recv.size = 1024 * 1024;
    }
    else if (ev == MG_EV_ACCEPT) {
        MG_INFO(("新连接：%p", c));
    }
    else if (ev == MG_EV_READ) {
        struct mg_str msg = mg_str_n((char *)c->recv.buf, c->recv.len);
        if (msg.len == 0) return;
        
        MG_INFO(("收到数据：%.*s", (int)msg.len, msg.buf));
        
        // 解析 COMMAND
        int cmd_len = 0;
        int cmd_off = mg_json_get(msg, "$.COMMAND", &cmd_len);
        if (cmd_off < 0) {
            send_error(c, 12, "无效的 JSON 格式", "UNKNOWN");
            mg_iobuf_del(&c->recv, 0, c->recv.len);
            return;
        }
        
        struct mg_str cmd = mg_str_n(msg.buf + cmd_off, cmd_len);
        
        // 解析 data 字段
        int data_len = 0;
        int data_off = mg_json_get(msg, "$.data", &data_len);
        struct mg_str data = mg_str_n(msg.buf + data_off, data_len);
        
        MG_INFO(("命令：%.*s", cmd_len, cmd.buf));
        
        // 根据命令分发处理
        if (mg_strcasecmp(cmd, mg_str("SIGNUP_ACCEPT")) == 0) {
            handle_signup_accept(c, data);
        }
        else if (mg_strcasecmp(cmd, mg_str("SIGNUP_CHECK")) == 0) {
            handle_signup_check(c, data);
        }
        else if (mg_strcasecmp(cmd, mg_str("AUTH_REQUEST")) == 0) {
            handle_auth_request(c, data);
        }
        else if (mg_strcasecmp(cmd, mg_str("AUTH_RESPONSE")) == 0) {
            handle_auth_response(c, data);
        }
        else if (mg_strcasecmp(cmd, mg_str("HEARTBEAT_REQUEST")) == 0) {
            handle_heartbeat_request(c, data);
        }
        else if (mg_strcasecmp(cmd, mg_str("CONTACTS_REQUEST")) == 0) {
            handle_contacts_request(c, data);
        }
        else if (mg_strcasecmp(cmd, mg_str("CHAT_SEND")) == 0) {
            handle_chat_send(c, data);
        }
        else if (mg_strcasecmp(cmd, mg_str("FILE_TRANSFER_START")) == 0) {
            handle_file_transfer_start(c, data);
        }
        else if (mg_strcasecmp(cmd, mg_str("FILE_TRANSFER_CHUNK")) == 0) {
            handle_file_transfer_chunk(c, data);
        }
        else if (mg_strcasecmp(cmd, mg_str("FILE_TRANSFER_END")) == 0) {
            handle_file_transfer_end(c, data);
        }
        else {
            char cmd_copy[64];
            strncpy(cmd_copy, cmd.buf, cmd_len);
            cmd_copy[cmd_len] = '\0';
            send_error(c, 13, "未知命令", cmd_copy);
        }
        
        // 清空接收缓冲区
        mg_iobuf_del(&c->recv, 0, c->recv.len);
    }
    else if (ev == MG_EV_CLOSE) {
        MG_INFO(("连接关闭：%p", c));
        client_session_t *session = find_client(c);
        if (session) {
            // 清理联系人
            if (session->contacts) {
                free(session->contacts);
                session->contacts = NULL;
                session->contact_capacity = 0;
                session->contact_count = 0;
            }
            
            // 清理文件传输
            if (session->pending_files) {
                for (int i = 0; i < session->pending_file_capacity; i++) {
                    if (session->pending_files[i].active && session->pending_files[i].buffer) {
                        free(session->pending_files[i].buffer);
                    }
                }
                free(session->pending_files);
                session->pending_files = NULL;
                session->pending_file_capacity = 0;
            }
            
            session->conn = NULL;
            session->state = CLIENT_STATE_INIT;
        }
    }
    else if (ev == MG_EV_ERROR) {
        MG_ERROR(("错误：%s", (char *)ev_data));
    }
}

// ==================== 配置加载函数 ====================

static void load_config(const char *config_path) {
    struct mg_str json = mg_file_read(&mg_fs_posix, config_path);
    if (json.buf == NULL) {
        MG_INFO(("未找到配置文件 %s，使用默认配置", config_path));
        return;
    }
    
    MG_INFO(("加载配置文件：%s", config_path));
    
    double val;
    if (mg_json_get_num(json, "$.port", &val)) {
        g_config.port = (int)val;
    }
    if (mg_json_get_num(json, "$.log_level", &val)) {
        g_config.log_level = (int)val;
    }
    if (mg_json_get_num(json, "$.max_clients", &val)) {
        g_config.max_clients = (int)val;
    }
    if (mg_json_get_num(json, "$.heartbeat_timeout", &val)) {
        g_config.heartbeat_timeout = (int)val;
    }
    if (mg_json_get_num(json, "$.default_contact_capacity", &val)) {
        g_config.default_contact_capacity = (int)val;
    }
    if (mg_json_get_num(json, "$.default_pending_file_capacity", &val)) {
        g_config.default_pending_file_capacity = (int)val;
    }
    
    MG_INFO(("配置：端口=%d, 日志级别=%d, 最大客户端=%d, 心跳超时=%d 秒，联系人容量=%d, 文件传输容量=%d",
             g_config.port, g_config.log_level, g_config.max_clients, g_config.heartbeat_timeout,
             g_config.default_contact_capacity, g_config.default_pending_file_capacity));
}

// ==================== 辅助函数 ====================

static void generate_token(char *buf, size_t len) {
    const char *chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    for (size_t i = 0; i < len - 1; i++) {
        buf[i] = chars[rand() % strlen(chars)];
    }
    buf[len - 1] = '\0';
}

static client_session_t *find_client(struct mg_connection *c) {
    for (int i = 0; i < g_config.max_clients; i++) {
        if (g_state.clients[i].conn == c) {
            return &g_state.clients[i];
        }
    }
    return NULL;
}

static client_session_t *create_client(struct mg_connection *c) {
    for (int i = 0; i < g_config.max_clients; i++) {
        if (g_state.clients[i].conn == NULL) {
            g_state.clients[i].conn = c;
            g_state.clients[i].state = CLIENT_STATE_INIT;
            g_state.clients[i].client_id = ++g_state.next_client_id;
            g_state.clients[i].last_heartbeat = time(NULL);
            
            // 初始化动态数组
            g_state.clients[i].contacts = calloc(g_config.default_contact_capacity, sizeof(contact_t));
            g_state.clients[i].contact_capacity = g_config.default_contact_capacity;
            g_state.clients[i].contact_count = 0;
            
            g_state.clients[i].pending_files = calloc(g_config.default_pending_file_capacity, sizeof(file_transfer_t));
            g_state.clients[i].pending_file_capacity = g_config.default_pending_file_capacity;
            g_state.clients[i].pending_file_count = 0;
            
            return &g_state.clients[i];
        }
    }
    return NULL;
}

static void send_json_response(struct mg_connection *c, const char *command, const char *data) {
    char buf[4096];
    snprintf(buf, sizeof(buf), "{\"COMMAND\":\"%s\",\"data\":%s}", command, data);
    mg_send(c, buf, strlen(buf));
    MG_INFO(("SEND: %s", buf));
}

static void send_error(struct mg_connection *c, int code, const char *msg, const char *failed_cmd) {
    char data[256];
    snprintf(data, sizeof(data), "{\"error_code\":%d,\"error_msg\":\"%s\",\"command\":\"%s\"}", 
             code, msg, failed_cmd);
    send_json_response(c, "ERROR", data);
}

// ==================== 动态数组管理 ====================

static void resize_contacts(client_session_t *session, int new_capacity) {
    if (new_capacity <= session->contact_capacity) return;
    
    contact_t *new_contacts = realloc(session->contacts, new_capacity * sizeof(contact_t));
    if (!new_contacts) {
        MG_ERROR(("联系人数组扩容失败"));
        return;
    }
    session->contacts = new_contacts;
    session->contact_capacity = new_capacity;
}

static void resize_pending_files(client_session_t *session, int new_capacity) {
    if (new_capacity <= session->pending_file_capacity) return;
    
    file_transfer_t *new_files = realloc(session->pending_files, new_capacity * sizeof(file_transfer_t));
    if (!new_files) {
        MG_ERROR(("文件传输列表扩容失败"));
        return;
    }
    session->pending_files = new_files;
    session->pending_file_capacity = new_capacity;
}

static file_transfer_t *find_or_create_transfer(client_session_t *session, const char *file_id) {
    for (int i = 0; i < session->pending_file_capacity; i++) {
        if (session->pending_files[i].active && 
            strcmp(session->pending_files[i].file_id, file_id) == 0) {
            return &session->pending_files[i];
        }
    }
    for (int i = 0; i < session->pending_file_capacity; i++) {
        if (!session->pending_files[i].active) {
            file_transfer_t *ft = &session->pending_files[i];
            memset(ft, 0, sizeof(file_transfer_t));
            ft->active = 1;
            strncpy(ft->file_id, file_id, sizeof(ft->file_id) - 1);
            return ft;
        }
    }
    int old_capacity = session->pending_file_capacity;
    resize_pending_files(session, session->pending_file_capacity * 2);
    for (int i = old_capacity; i < session->pending_file_capacity; i++) {
        if (!session->pending_files[i].active) {
            file_transfer_t *ft = &session->pending_files[i];
            memset(ft, 0, sizeof(file_transfer_t));
            ft->active = 1;
            strncpy(ft->file_id, file_id, sizeof(ft->file_id) - 1);
            return ft;
        }
    }
    return NULL;
}

static file_transfer_t *find_transfer(client_session_t *session, const char *file_id) {
    for (int i = 0; i < session->pending_file_capacity; i++) {
        if (session->pending_files[i].active && 
            strcmp(session->pending_files[i].file_id, file_id) == 0) {
            return &session->pending_files[i];
        }
    }
    return NULL;
}

static void free_transfer(file_transfer_t *ft) {
    if (ft && ft->buffer) {
        free(ft->buffer);
        ft->buffer = NULL;
    }
    memset(ft, 0, sizeof(file_transfer_t));
}

// ==================== 解压函数 ====================

static unsigned char *decompress_data(const unsigned char *input, size_t input_len, size_t *output_len) {
    *output_len = 0;
    
    void *decompressed = tinfl_decompress_mem_to_heap(input, input_len, output_len, 0);
    if (decompressed == NULL) {
        MG_ERROR(("解压失败"));
        return NULL;
    }
    
    MG_INFO(("解压成功：输入=%zu 字节，输出=%zu 字节", input_len, *output_len));
    return (unsigned char *)decompressed;
}

// ==================== 命令处理函数 ====================

static void handle_signup_accept(struct mg_connection *c, struct mg_str data) {
    client_session_t *session = find_client(c);
    if (!session) {
        session = create_client(c);
        if (!session) {
            send_error(c, 1, "客户端数量已达上限", "SIGNUP_ACCEPT");
            return;
        }
    }
    
    char *pub_key = mg_json_get_str(data, "$.pub_key");
    if (!pub_key) {
        send_error(c, 2, "缺少公钥", "SIGNUP_ACCEPT");
        return;
    }
    MG_INFO(("收到注册公钥：%s", pub_key));
    free(pub_key);
    
    char challenge[33];
    generate_token(challenge, sizeof(challenge));
    
    session->state = CLIENT_STATE_SIGNUP;
    
    char data_buf[256];
    snprintf(data_buf, sizeof(data_buf), "{\"check_data\":\"%s\"}", challenge);
    send_json_response(c, "SIGNUP_CHECK", data_buf);
}

static void handle_signup_check(struct mg_connection *c, struct mg_str data) {
    client_session_t *session = find_client(c);
    if (!session || session->state != CLIENT_STATE_SIGNUP) {
        send_error(c, 3, "注册流程错误", "SIGNUP_CHECK");
        return;
    }
    
    char *signature = mg_json_get_str(data, "$.check_data");
    if (!signature) {
        send_error(c, 4, "缺少签名数据", "SIGNUP_CHECK");
        return;
    }
    MG_INFO(("收到注册签名：%s", signature));
    free(signature);
    
    session->state = CLIENT_STATE_AUTHED;
    generate_token(session->chat_token, sizeof(session->chat_token));
    
    char data_buf[64];
    snprintf(data_buf, sizeof(data_buf), "{\"id\":%d}", session->client_id);
    send_json_response(c, "SIGNUP_AGREE", data_buf);
    
    MG_INFO(("用户 %d 注册成功，令牌：%s", session->client_id, session->chat_token));
}

static void handle_auth_request(struct mg_connection *c, struct mg_str data) {
    client_session_t *session = find_client(c);
    if (!session) {
        session = create_client(c);
        if (!session) {
            send_error(c, 1, "客户端数量已达上限", "AUTH_REQUEST");
            return;
        }
    }
    
    char *client_id_str = mg_json_get_str(data, "$.client_id");
    if (!client_id_str) {
        send_error(c, 5, "缺少客户端 ID", "AUTH_REQUEST");
        return;
    }
    
    int client_id = atoi(client_id_str);
    free(client_id_str);
    MG_INFO(("认证请求：client_id=%d", client_id));
    
    char challenge[33];
    generate_token(challenge, sizeof(challenge));
    
    session->client_id = client_id;
    session->state = CLIENT_STATE_SIGNUP;
    
    char data_buf[256];
    snprintf(data_buf, sizeof(data_buf), "{\"challenge_data\":\"%s\"}", challenge);
    send_json_response(c, "AUTH_CHALLENGE", data_buf);
}

static void handle_auth_response(struct mg_connection *c, struct mg_str data) {
    client_session_t *session = find_client(c);
    if (!session || session->state != CLIENT_STATE_SIGNUP) {
        send_error(c, 6, "认证流程错误", "AUTH_RESPONSE");
        return;
    }
    
    char *signature = mg_json_get_str(data, "$.signature");
    if (!signature) {
        send_error(c, 7, "缺少签名", "AUTH_RESPONSE");
        return;
    }
    MG_INFO(("收到认证签名：%s", signature));
    free(signature);
    
    session->state = CLIENT_STATE_AUTHED;
    generate_token(session->chat_token, sizeof(session->chat_token));
    
    const char *contacts_json = 
        "[{\"id\":1,\"name\":\"user1\",\"online\":true},"
        "{\"id\":2,\"name\":\"user2\",\"online\":false}]";
    
    char data_buf[512];
    snprintf(data_buf, sizeof(data_buf), 
             "{\"chat_token\":\"%s\",\"contacts\":%s}", 
             session->chat_token, contacts_json);
    send_json_response(c, "AUTH_SUCCESS", data_buf);
    
    MG_INFO(("用户 %d 认证成功", session->client_id));
}

static void handle_heartbeat_request(struct mg_connection *c, struct mg_str data) {
    client_session_t *session = find_client(c);
    if (!session || session->state != CLIENT_STATE_AUTHED) {
        send_error(c, 8, "未认证", "HEARTBEAT_REQUEST");
        return;
    }
    
    session->last_heartbeat = time(NULL);
    
    double timestamp;
    if (!mg_json_get_num(data, "$.timestamp", &timestamp)) {
        timestamp = (double)time(NULL);
    }
    
    char data_buf[256];
    snprintf(data_buf, sizeof(data_buf), 
             "{\"timestamp\":%.0f,\"contacts_update\":[]}", timestamp);
    send_json_response(c, "HEARTBEAT_RESPONSE", data_buf);
}

static void handle_contacts_request(struct mg_connection *c, struct mg_str data) {
    client_session_t *session = find_client(c);
    if (!session || session->state != CLIENT_STATE_AUTHED) {
        send_error(c, 9, "未认证", "CONTACTS_REQUEST");
        return;
    }
    
    const char *contacts_json = 
        "[{\"id\":1,\"name\":\"user1\",\"online\":true},"
        "{\"id\":2,\"name\":\"user2\",\"online\":false}]";
    
    char data_buf[512];
    snprintf(data_buf, sizeof(data_buf), "{\"contacts\":%s}", contacts_json);
    send_json_response(c, "CONTACTS_RESPONSE", data_buf);
}

static void handle_chat_send(struct mg_connection *c, struct mg_str data) {
    client_session_t *session = find_client(c);
    if (!session || session->state != CLIENT_STATE_AUTHED) {
        send_error(c, 10, "未认证", "CHAT_SEND");
        return;
    }
    
    int target_id = (int)mg_json_get_long(data, "$.target_id", -1);
    char *chat_token = mg_json_get_str(data, "$.chat_token");
    char *content_type = mg_json_get_str(data, "$.content_type");
    int content_len = 0;
    char *content = mg_json_get_b64(data, "$.content", &content_len);
    
    if (target_id < 0 || !chat_token || !content_type || !content) {
        send_error(c, 11, "缺少必要字段", "CHAT_SEND");
        if (chat_token) free(chat_token);
        if (content_type) free(content_type);
        if (content) free(content);
        return;
    }
    
    MG_INFO(("收到消息：target_id=%d, type=%s, len=%d", target_id, content_type, content_len));
    
    if (strcmp(content_type, "bin") == 0) {
        size_t decompressed_len = 0;
        unsigned char *decompressed = decompress_data((unsigned char *)content, content_len, &decompressed_len);
        if (decompressed) {
            MG_INFO(("二进制内容解压成功：%zu 字节", decompressed_len));
            free(decompressed);
        }
    }
    
    free(chat_token);
    free(content_type);
    free(content);
    
    char data_buf[128];
    snprintf(data_buf, sizeof(data_buf), 
             "{\"file_id\":\"file_%ld\",\"status\":\"success\",\"msg\":\"\"}", time(NULL));
    send_json_response(c, "SEND_FILE_ACK", data_buf);
}

// ==================== 分块文件传输处理函数 ====================

static void handle_file_transfer_start(struct mg_connection *c, struct mg_str data) {
    client_session_t *session = find_client(c);
    if (!session || session->state != CLIENT_STATE_AUTHED) {
        send_error(c, 10, "未认证", "FILE_TRANSFER_START");
        return;
    }
    
    char *file_id = mg_json_get_str(data, "$.file_id");
    char *file_name = mg_json_get_str(data, "$.file_name");
    double file_size_d;
    double chunk_size_d;
    double total_chunks_d;
    char *content_type = mg_json_get_str(data, "$.content_type");
    
    if (!file_id || !file_name || 
        !mg_json_get_num(data, "$.file_size", &file_size_d) ||
        !mg_json_get_num(data, "$.chunk_size", &chunk_size_d) ||
        !mg_json_get_num(data, "$.total_chunks", &total_chunks_d) ||
        !content_type) {
        send_error(c, 14, "缺少必要字段", "FILE_TRANSFER_START");
        if (file_id) free(file_id);
        if (file_name) free(file_name);
        if (content_type) free(content_type);
        return;
    }
    
    file_transfer_t *ft = find_or_create_transfer(session, file_id);
    if (!ft) {
        send_error(c, 15, "文件传输会话已达上限", "FILE_TRANSFER_START");
        free(file_id);
        free(file_name);
        free(content_type);
        return;
    }
    
    strncpy(ft->file_name, file_name, sizeof(ft->file_name) - 1);
    ft->file_size = (uint32_t)file_size_d;
    ft->chunk_size = (uint32_t)chunk_size_d;
    ft->total_chunks = (int)total_chunks_d;
    ft->received_chunks = 0;
    strncpy(ft->content_type, content_type, sizeof(ft->content_type) - 1);
    ft->start_time = time(NULL);
    
    ft->buffer = calloc(1, ft->file_size);
    if (!ft->buffer) {
        send_error(c, 16, "内存分配失败", "FILE_TRANSFER_START");
        free_transfer(ft);
        free(file_id);
        free(file_name);
        free(content_type);
        return;
    }
    ft->buffer_size = ft->file_size;
    
    MG_INFO(("文件传输开始：file_id=%s, name=%s, size=%u, chunks=%d", 
             file_id, file_name, ft->file_size, ft->total_chunks));
    
    free(file_id);
    free(file_name);
    free(content_type);
    
    char data_buf[128];
    snprintf(data_buf, sizeof(data_buf), 
             "{\"file_id\":\"%s\",\"status\":\"ready\",\"next_chunk\":0}", ft->file_id);
    send_json_response(c, "FILE_TRANSFER_ACK", data_buf);
}

static void handle_file_transfer_chunk(struct mg_connection *c, struct mg_str data) {
    client_session_t *session = find_client(c);
    if (!session || session->state != CLIENT_STATE_AUTHED) {
        send_error(c, 10, "未认证", "FILE_TRANSFER_CHUNK");
        return;
    }
    
    char *file_id = mg_json_get_str(data, "$.file_id");
    double chunk_index_d;
    double chunk_size_d;
    int content_len = 0;
    char *content = mg_json_get_b64(data, "$.content", &content_len);
    
    if (!file_id || !mg_json_get_num(data, "$.chunk_index", &chunk_index_d) || 
        !mg_json_get_num(data, "$.chunk_size", &chunk_size_d) || !content) {
        send_error(c, 14, "缺少必要字段", "FILE_TRANSFER_CHUNK");
        if (file_id) free(file_id);
        if (content) free(content);
        return;
    }
    
    int chunk_index = (int)chunk_index_d;
    
    file_transfer_t *ft = find_transfer(session, file_id);
    if (!ft) {
        send_error(c, 17, "文件传输会话不存在", "FILE_TRANSFER_CHUNK");
        free(file_id);
        free(content);
        return;
    }
    
    if (chunk_index < 0 || chunk_index >= ft->total_chunks) {
        send_error(c, 18, "无效的块索引", "FILE_TRANSFER_CHUNK");
        free(file_id);
        free(content);
        return;
    }
    
    uint32_t offset = chunk_index * ft->chunk_size;
    uint32_t copy_size = (chunk_index == ft->total_chunks - 1) ? 
                         (ft->file_size - offset) : ft->chunk_size;
    
    if ((uint32_t)content_len > copy_size) {
        content_len = copy_size;
    }
    
    if (offset + content_len <= ft->buffer_size) {
        memcpy(ft->buffer + offset, content, content_len);
        ft->received_chunks++;
        
        MG_INFO(("收到文件块：%s, chunk=%d/%d, size=%d", 
                 file_id, chunk_index, ft->total_chunks, content_len));
    } else {
        MG_ERROR(("缓冲区溢出：offset=%u, len=%d, buffer_size=%u", 
                  offset, content_len, ft->buffer_size));
    }
    
    free(file_id);
    free(content);
    
    char data_buf[128];
    snprintf(data_buf, sizeof(data_buf), 
             "{\"file_id\":\"%s\",\"chunk_index\":%d,\"status\":\"received\",\"next_chunk\":%d}",
             ft->file_id, chunk_index, chunk_index + 1);
    send_json_response(c, "FILE_TRANSFER_CHUNK_ACK", data_buf);
}

static void handle_file_transfer_end(struct mg_connection *c, struct mg_str data) {
    client_session_t *session = find_client(c);
    if (!session || session->state != CLIENT_STATE_AUTHED) {
        send_error(c, 10, "未认证", "FILE_TRANSFER_END");
        return;
    }
    
    char *file_id = mg_json_get_str(data, "$.file_id");
    char *checksum = mg_json_get_str(data, "$.checksum");
    
    if (!file_id) {
        send_error(c, 14, "缺少 file_id", "FILE_TRANSFER_END");
        if (checksum) free(checksum);
        return;
    }
    
    file_transfer_t *ft = find_transfer(session, file_id);
    if (!ft) {
        send_error(c, 17, "文件传输会话不存在", "FILE_TRANSFER_END");
        free(file_id);
        if (checksum) free(checksum);
        return;
    }
    
    if (ft->received_chunks != ft->total_chunks) {
        char err_msg[128];
        snprintf(err_msg, sizeof(err_msg), "缺少 %d 个块", ft->total_chunks - ft->received_chunks);
        send_error(c, 19, err_msg, "FILE_TRANSFER_END");
        free_transfer(ft);
        free(file_id);
        if (checksum) free(checksum);
        return;
    }
    
    MG_INFO(("文件传输完成：%s, 总大小=%u, 接收块数=%d", 
             file_id, ft->file_size, ft->received_chunks));
    
    if (strcmp(ft->content_type, "bin") == 0) {
        size_t decompressed_len = 0;
        unsigned char *decompressed = decompress_data(ft->buffer, ft->file_size, &decompressed_len);
        if (decompressed) {
            MG_INFO(("文件解压成功：原始=%u 字节，解压后=%zu 字节", ft->file_size, decompressed_len));
            free(decompressed);
        }
    }
    
    char data_buf[128];
    snprintf(data_buf, sizeof(data_buf), 
             "{\"file_id\":\"%s\",\"status\":\"success\",\"msg\":\"文件接收完成\"}", file_id);
    send_json_response(c, "FILE_TRANSFER_COMPLETE", data_buf);
    
    free_transfer(ft);
    free(file_id);
    if (checksum) free(checksum);
}
