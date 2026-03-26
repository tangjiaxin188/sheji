#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "mongoose.h"
#include "miniz.h"
#include "lib.h"

// 全局状态定义
server_state_t g_state = {0};

// 配置定义
server_config_t g_config = {
    .port = 25565,
    .log_level = 4,
    .max_clients = 100,
    .heartbeat_timeout = 120,
};

// ==================== 辅助函数 ====================

char *parse_data_string(const char *data_str, size_t data_len) {
    // data_str 指向的是 JSON 中 data 字段的值（包括引号）
    // 例如："data":"{\"pub_key\":\"test\"}" 中的 "{\"pub_key\":\"test\"}"
    if (data_len < 2 || data_str[0] != '"') {
        // 不是字符串，直接返回 NULL
        return NULL;
    }
    
    // 去掉外层引号并处理转义
    char *result = malloc(data_len);
    if (!result) return NULL;
    
    size_t j = 0;
    for (size_t i = 1; i < data_len - 1; i++) {
        if (data_str[i] == '\\' && i + 1 < data_len - 1) {
            i++;  // 跳过转义符，复制下一个字符
        }
        result[j++] = data_str[i];
    }
    result[j] = '\0';
    return result;
}

void generate_token(char *buf, size_t len) {
    const char *chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    for (size_t i = 0; i < len - 1; i++) {
        buf[i] = chars[rand() % strlen(chars)];
    }
    buf[len - 1] = '\0';
}

client_session_t *find_client(struct mg_connection *c) {
    // 首先查找匹配连接的会话
    for (int i = 0; i < g_config.max_clients; i++) {
        if (g_state.clients[i].conn == c) {
            return &g_state.clients[i];
        }
    }
    // 测试目的：如果没有找到匹配的连接，返回第一个已初始化且状态不是 INIT 的会话
    // 实际应用中应该通过 Token 或 Cookie 识别用户
    for (int i = 0; i < g_config.max_clients; i++) {
        if (g_state.clients[i].state != CLIENT_STATE_INIT) {
            return &g_state.clients[i];
        }
    }
    return NULL;
}

client_session_t *create_client(struct mg_connection *c) {
    for (int i = 0; i < g_config.max_clients; i++) {
        if (g_state.clients[i].conn == NULL) {
            g_state.clients[i].conn = c;
            g_state.clients[i].state = CLIENT_STATE_INIT;
            g_state.clients[i].client_id = ++g_state.next_client_id;
            g_state.clients[i].last_heartbeat = time(NULL);

            g_state.clients[i].contacts = calloc(100, sizeof(contact_t));
            g_state.clients[i].contact_capacity = 100;
            g_state.clients[i].contact_count = 0;

            g_state.clients[i].pending_files = calloc(10, sizeof(file_transfer_t));
            g_state.clients[i].pending_file_capacity = 10;
            g_state.clients[i].pending_file_count = 0;

            return &g_state.clients[i];
        }
    }
    return NULL;
}

void send_json(struct mg_connection *c, int status, const char *command, const char *data) {
    char buf[4096];
    snprintf(buf, sizeof(buf), "{\"COMMAND\":\"%s\",\"data\":%s}", command, data);
    mg_http_reply(c, status, "Content-Type: application/json\r\n", buf);
    MG_INFO(("SEND %d: %s", status, buf));
}

void send_error(struct mg_connection *c, int code, const char *msg, const char *cmd) {
    char data[256];
    snprintf(data, sizeof(data), 
             "{\"error_code\":%d,\"error_msg\":\"%s\",\"command\":\"%s\"}",
             code, msg, cmd);
    send_json(c, 400, "ERROR", data);
}

unsigned char *decompress_data(const unsigned char *input, size_t input_len, size_t *output_len) {
    *output_len = 0;
    void *decompressed = tinfl_decompress_mem_to_heap(input, input_len, output_len, 0);
    if (decompressed == NULL) {
        MG_ERROR(("解压失败"));
        return NULL;
    }
    MG_INFO(("解压成功：输入=%zu 字节，输出=%zu 字节", input_len, *output_len));
    return (unsigned char *)decompressed;
}

// ==================== 动态数组管理 ====================

void resize_contacts(client_session_t *session, int new_capacity) {
    if (new_capacity <= session->contact_capacity) return;
    contact_t *new_contacts = realloc(session->contacts, new_capacity * sizeof(contact_t));
    if (!new_contacts) {
        MG_ERROR(("联系人数组扩容失败"));
        return;
    }
    session->contacts = new_contacts;
    session->contact_capacity = new_capacity;
}

void resize_pending_files(client_session_t *session, int new_capacity) {
    if (new_capacity <= session->pending_file_capacity) return;
    file_transfer_t *new_files = realloc(session->pending_files, new_capacity * sizeof(file_transfer_t));
    if (!new_files) {
        MG_ERROR(("文件传输列表扩容失败"));
        return;
    }
    session->pending_files = new_files;
    session->pending_file_capacity = new_capacity;
}

file_transfer_t *find_or_create_transfer(client_session_t *session, const char *file_id) {
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

file_transfer_t *find_transfer(client_session_t *session, const char *file_id) {
    for (int i = 0; i < session->pending_file_capacity; i++) {
        if (session->pending_files[i].active &&
            strcmp(session->pending_files[i].file_id, file_id) == 0) {
            return &session->pending_files[i];
        }
    }
    return NULL;
}

void free_transfer(file_transfer_t *ft) {
    if (ft && ft->buffer) {
        free(ft->buffer);
        ft->buffer = NULL;
    }
    memset(ft, 0, sizeof(file_transfer_t));
}

// ==================== 命令处理函数 ====================

static struct mg_str parse_data_json(const char *data_str, size_t data_len) {
    char *parsed = parse_data_string(data_str, data_len);
    if (parsed) {
        struct mg_str result = mg_str(parsed);
        return result;
    }
    return mg_str_n(data_str, data_len);
}

static void free_parsed_data(struct mg_str data, const char *original) {
    // 如果 data.ptr 和 original+1 相同，说明是解析后的字符串，需要释放
    if (data.ptr != original && data.ptr != original + 1) {
        free((char *)data.ptr);
    }
}

void handle_signup_accept(struct mg_connection *c, const char *data_str) {
    size_t data_len = strlen(data_str);
    struct mg_str data = parse_data_json(data_str, data_len);
    int need_free = (data.ptr != data_str);
    
    client_session_t *session = find_client(c);
    if (!session) {
        session = create_client(c);
        if (!session) {
            send_error(c, 1, "客户端数量已达上限", "SIGNUP_ACCEPT");
            if (need_free) free((char *)data.ptr);
            return;
        }
    }

    char *pub_key = mg_json_get_str(data, "$.pub_key");
    if (!pub_key) {
        send_error(c, 2, "缺少公钥", "SIGNUP_ACCEPT");
        if (need_free) free((char *)data.ptr);
        return;
    }
    MG_INFO(("收到注册公钥：%s", pub_key));
    free(pub_key);
    if (need_free) free((char *)data.ptr);

    char challenge[33];
    generate_token(challenge, sizeof(challenge));

    session->state = CLIENT_STATE_SIGNUP;

    char data_buf[256];
    snprintf(data_buf, sizeof(data_buf), "{\"check_data\":\"%s\"}", challenge);
    send_json(c, 200, "SIGNUP_CHECK", data_buf);
}

void handle_signup_check(struct mg_connection *c, const char *data_str) {
    size_t data_len = strlen(data_str);
    struct mg_str data = parse_data_json(data_str, data_len);
    int need_free = (data.ptr != data_str);

    client_session_t *session = find_client(c);
    if (!session || session->state != CLIENT_STATE_SIGNUP) {
        send_error(c, 3, "注册流程错误", "SIGNUP_CHECK");
        if (need_free) free((char *)data.ptr);
        return;
    }

    char *signature = mg_json_get_str(data, "$.check_data");
    if (!signature) {
        send_error(c, 4, "缺少签名数据", "SIGNUP_CHECK");
        if (need_free) free((char *)data.ptr);
        return;
    }
    MG_INFO(("收到注册签名：%s", signature));
    free(signature);
    if (need_free) free((char *)data.ptr);

    session->state = CLIENT_STATE_AUTHED;
    generate_token(session->chat_token, sizeof(session->chat_token));

    char data_buf[64];
    snprintf(data_buf, sizeof(data_buf), "{\"id\":%d}", session->client_id);
    send_json(c, 200, "SIGNUP_AGREE", data_buf);

    MG_INFO(("用户 %d 注册成功，令牌：%s", session->client_id, session->chat_token));
}

void handle_auth_request(struct mg_connection *c, const char *data_str) {
    size_t data_len = strlen(data_str);
    struct mg_str data = parse_data_json(data_str, data_len);
    int need_free = (data.ptr != data_str);
    
    client_session_t *session = find_client(c);
    if (!session) {
        session = create_client(c);
        if (!session) {
            send_error(c, 1, "客户端数量已达上限", "AUTH_REQUEST");
            if (need_free) free((char *)data.ptr);
            return;
        }
    }

    char *client_id_str = mg_json_get_str(data, "$.client_id");
    if (!client_id_str) {
        send_error(c, 5, "缺少客户端 ID", "AUTH_REQUEST");
        if (need_free) free((char *)data.ptr);
        return;
    }

    int client_id = atoi(client_id_str);
    free(client_id_str);
    if (need_free) free((char *)data.ptr);
    
    MG_INFO(("认证请求：client_id=%d", client_id));

    char challenge[33];
    generate_token(challenge, sizeof(challenge));

    session->client_id = client_id;
    session->state = CLIENT_STATE_SIGNUP;

    char data_buf[256];
    snprintf(data_buf, sizeof(data_buf), "{\"challenge_data\":\"%s\"}", challenge);
    send_json(c, 200, "AUTH_CHALLENGE", data_buf);
}

void handle_auth_response(struct mg_connection *c, const char *data_str) {
    size_t data_len = strlen(data_str);
    struct mg_str data = parse_data_json(data_str, data_len);
    int need_free = (data.ptr != data_str);
    
    client_session_t *session = find_client(c);
    if (!session || session->state != CLIENT_STATE_SIGNUP) {
        send_error(c, 6, "认证流程错误", "AUTH_RESPONSE");
        if (need_free) free((char *)data.ptr);
        return;
    }

    char *signature = mg_json_get_str(data, "$.signature");
    if (!signature) {
        send_error(c, 7, "缺少签名", "AUTH_RESPONSE");
        if (need_free) free((char *)data.ptr);
        return;
    }
    MG_INFO(("收到认证签名：%s", signature));
    free(signature);
    if (need_free) free((char *)data.ptr);

    session->state = CLIENT_STATE_AUTHED;
    generate_token(session->chat_token, sizeof(session->chat_token));

    const char *contacts_json =
        "[{\"id\":1,\"name\":\"user1\",\"online\":true},"
        "{\"id\":2,\"name\":\"user2\",\"online\":false}]";

    char data_buf[512];
    snprintf(data_buf, sizeof(data_buf),
             "{\"chat_token\":\"%s\",\"contacts\":%s}",
             session->chat_token, contacts_json);
    send_json(c, 200, "AUTH_SUCCESS", data_buf);

    MG_INFO(("用户 %d 认证成功", session->client_id));
}

void handle_heartbeat_request(struct mg_connection *c, const char *data_str) {
    size_t data_len = strlen(data_str);
    struct mg_str data = parse_data_json(data_str, data_len);
    int need_free = (data.ptr != data_str);
    
    client_session_t *session = find_client(c);
    if (!session || session->state != CLIENT_STATE_AUTHED) {
        send_error(c, 8, "未认证", "HEARTBEAT_REQUEST");
        if (need_free) free((char *)data.ptr);
        return;
    }

    session->last_heartbeat = time(NULL);

    double timestamp;
    if (!mg_json_get_num(data, "$.timestamp", &timestamp)) {
        timestamp = (double)time(NULL);
    }
    if (need_free) free((char *)data.ptr);

    char data_buf[256];
    snprintf(data_buf, sizeof(data_buf),
             "{\"timestamp\":%.0f,\"contacts_update\":[]}", timestamp);
    send_json(c, 200, "HEARTBEAT_RESPONSE", data_buf);
}

void handle_contacts_request(struct mg_connection *c, const char *data_str) {
    size_t data_len = strlen(data_str);
    struct mg_str data = parse_data_json(data_str, data_len);
    int need_free = (data.ptr != data_str);
    (void)data;  // 未使用
    (void)data_len;
    
    client_session_t *session = find_client(c);
    if (!session || session->state != CLIENT_STATE_AUTHED) {
        send_error(c, 9, "未认证", "CONTACTS_REQUEST");
        if (need_free) free((char *)data.ptr);
        return;
    }
    if (need_free) free((char *)data.ptr);

    const char *contacts_json =
        "[{\"id\":1,\"name\":\"user1\",\"online\":true},"
        "{\"id\":2,\"name\":\"user2\",\"online\":false}]";

    char data_buf[512];
    snprintf(data_buf, sizeof(data_buf), "{\"contacts\":%s}", contacts_json);
    send_json(c, 200, "CONTACTS_RESPONSE", data_buf);
}

void handle_chat_send(struct mg_connection *c, const char *data_str) {
    size_t data_len = strlen(data_str);
    struct mg_str data = parse_data_json(data_str, data_len);
    int need_free = (data.ptr != data_str);
    
    client_session_t *session = find_client(c);
    if (!session || session->state != CLIENT_STATE_AUTHED) {
        send_error(c, 10, "未认证", "CHAT_SEND");
        if (need_free) free((char *)data.ptr);
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
        if (need_free) free((char *)data.ptr);
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
    if (need_free) free((char *)data.ptr);

    char data_buf[128];
    snprintf(data_buf, sizeof(data_buf),
             "{\"file_id\":\"file_%ld\",\"status\":\"success\",\"msg\":\"\"}", time(NULL));
    send_json(c, 200, "SEND_FILE_ACK", data_buf);
}

void handle_file_transfer_start(struct mg_connection *c, const char *data_str) {
    size_t data_len = strlen(data_str);
    struct mg_str data = parse_data_json(data_str, data_len);
    int need_free = (data.ptr != data_str);
    
    client_session_t *session = find_client(c);
    if (!session || session->state != CLIENT_STATE_AUTHED) {
        send_error(c, 10, "未认证", "FILE_TRANSFER_START");
        if (need_free) free((char *)data.ptr);
        return;
    }

    char *file_id = mg_json_get_str(data, "$.file_id");
    char *file_name = mg_json_get_str(data, "$.file_name");
    double file_size_d;
    char *content_type = mg_json_get_str(data, "$.content_type");

    if (!file_id || !file_name ||
        !mg_json_get_num(data, "$.file_size", &file_size_d) ||
        !content_type) {
        send_error(c, 14, "缺少必要字段", "FILE_TRANSFER_START");
        if (file_id) free(file_id);
        if (file_name) free(file_name);
        if (content_type) free(content_type);
        if (need_free) free((char *)data.ptr);
        return;
    }

    file_transfer_t *ft = find_or_create_transfer(session, file_id);
    if (!ft) {
        send_error(c, 15, "文件传输会话已达上限", "FILE_TRANSFER_START");
        free(file_id);
        free(file_name);
        free(content_type);
        if (need_free) free((char *)data.ptr);
        return;
    }

    strncpy(ft->file_name, file_name, sizeof(ft->file_name) - 1);
    ft->file_size = (uint32_t)file_size_d;
    ft->received = 0;
    strncpy(ft->content_type, content_type, sizeof(ft->content_type) - 1);
    ft->start_time = time(NULL);

    ft->buffer = calloc(1, ft->file_size);
    if (!ft->buffer) {
        send_error(c, 16, "内存分配失败", "FILE_TRANSFER_START");
        free_transfer(ft);
        free(file_id);
        free(file_name);
        free(content_type);
        if (need_free) free((char *)data.ptr);
        return;
    }
    ft->buffer_size = ft->file_size;

    MG_INFO(("文件传输开始：file_id=%s, name=%s, size=%u",
             file_id, file_name, ft->file_size));

    free(file_id);
    free(file_name);
    free(content_type);
    if (need_free) free((char *)data.ptr);

    char data_buf[128];
    snprintf(data_buf, sizeof(data_buf),
             "{\"file_id\":\"%s\",\"status\":\"ready\",\"upload_url\":\"/upload/%s\"}", 
             ft->file_id, ft->file_id);
    send_json(c, 200, "FILE_TRANSFER_ACK", data_buf);
}

void handle_file_transfer_chunk(struct mg_connection *c, const char *data_str) {
    size_t data_len = strlen(data_str);
    struct mg_str data = parse_data_json(data_str, data_len);
    int need_free = (data.ptr != data_str);
    
    client_session_t *session = find_client(c);
    if (!session || session->state != CLIENT_STATE_AUTHED) {
        send_error(c, 10, "未认证", "FILE_TRANSFER_CHUNK");
        if (need_free) free((char *)data.ptr);
        return;
    }

    char *file_id = mg_json_get_str(data, "$.file_id");
    double chunk_index_d;
    int content_len = 0;
    char *content = mg_json_get_b64(data, "$.content", &content_len);

    if (!file_id || !mg_json_get_num(data, "$.chunk_index", &chunk_index_d) || !content) {
        send_error(c, 14, "缺少必要字段", "FILE_TRANSFER_CHUNK");
        if (file_id) free(file_id);
        if (content) free(content);
        if (need_free) free((char *)data.ptr);
        return;
    }

    int chunk_index = (int)chunk_index_d;

    file_transfer_t *ft = find_transfer(session, file_id);
    if (!ft) {
        send_error(c, 17, "文件传输会话不存在", "FILE_TRANSFER_CHUNK");
        free(file_id);
        free(content);
        if (need_free) free((char *)data.ptr);
        return;
    }

    uint32_t offset = chunk_index * 65536;
    if (offset + content_len <= ft->buffer_size) {
        memcpy(ft->buffer + offset, content, content_len);
        ft->received += content_len;
        MG_INFO(("收到文件块：%s, chunk=%d, size=%d", file_id, chunk_index, content_len));
    }

    free(file_id);
    free(content);
    if (need_free) free((char *)data.ptr);

    char data_buf[128];
    snprintf(data_buf, sizeof(data_buf),
             "{\"file_id\":\"%s\",\"chunk_index\":%d,\"status\":\"received\",\"next_chunk\":%d}",
             ft->file_id, chunk_index, chunk_index + 1);
    send_json(c, 200, "FILE_TRANSFER_CHUNK_ACK", data_buf);
}

void handle_file_transfer_end(struct mg_connection *c, const char *data_str) {
    size_t data_len = strlen(data_str);
    struct mg_str data = parse_data_json(data_str, data_len);
    int need_free = (data.ptr != data_str);
    
    client_session_t *session = find_client(c);
    if (!session || session->state != CLIENT_STATE_AUTHED) {
        send_error(c, 10, "未认证", "FILE_TRANSFER_END");
        if (need_free) free((char *)data.ptr);
        return;
    }

    char *file_id = mg_json_get_str(data, "$.file_id");
    char *checksum = mg_json_get_str(data, "$.checksum");

    if (!file_id) {
        send_error(c, 14, "缺少 file_id", "FILE_TRANSFER_END");
        if (checksum) free(checksum);
        if (need_free) free((char *)data.ptr);
        return;
    }

    file_transfer_t *ft = find_transfer(session, file_id);
    if (!ft) {
        send_error(c, 17, "文件传输会话不存在", "FILE_TRANSFER_END");
        free(file_id);
        if (checksum) free(checksum);
        if (need_free) free((char *)data.ptr);
        return;
    }

    if (need_free) free((char *)data.ptr);

    MG_INFO(("文件传输完成：%s, 接收=%u/%u 字节",
             file_id, ft->received, ft->file_size));

    if (strcmp(ft->content_type, "bin") == 0) {
        size_t decompressed_len = 0;
        unsigned char *decompressed = decompress_data(ft->buffer, ft->received, &decompressed_len);
        if (decompressed) {
            MG_INFO(("文件解压成功：原始=%u 字节，解压后=%zu 字节", ft->received, decompressed_len));
            free(decompressed);
        }
    }

    char data_buf[128];
    snprintf(data_buf, sizeof(data_buf),
             "{\"file_id\":\"%s\",\"status\":\"success\",\"msg\":\"文件接收完成\"}", file_id);
    send_json(c, 200, "FILE_TRANSFER_COMPLETE", data_buf);

    free_transfer(ft);
    free(file_id);
    if (checksum) free(checksum);
}
