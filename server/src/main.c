#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "mongoose.h"
#include "easy_miniz.h"
#include "lib.h"

// ==================== 函数声明 ====================

static void fn(struct mg_connection *c, int ev, void *ev_data, void *fn_data);

static void handle_api_request(struct mg_connection *c, struct mg_http_message *hm);
static void handle_upload(struct mg_connection *c, struct mg_http_message *hm);
static void load_config(const char *config_path);

// ==================== 主函数 ====================

int main(void) {
    struct mg_mgr mgr;
    struct mg_connection *c;
    char url[64];

    load_config("config.json");
    srand((unsigned int)time(NULL));

    g_state.clients = calloc(g_config.max_clients, sizeof(client_session_t));
    if (!g_state.clients) {
        MG_ERROR(("内存分配失败"));
        return 1;
    }

    mg_mgr_init(&mgr);
    snprintf(url, sizeof(url), "http://0.0.0.0:%d", g_config.port);
    c = mg_http_listen(&mgr, url, fn, NULL);
    if (!c) {
        MG_ERROR(("无法创建监听器"));
        free(g_state.clients);
        return 1;
    }

    MG_INFO(("HTTP 服务器启动，监听端口 %d", g_config.port));

    for (;;) {
        mg_mgr_poll(&mgr, 1000);

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

    for (int i = 0; i < g_config.max_clients; i++) {
        if (g_state.clients[i].contacts) free(g_state.clients[i].contacts);
        if (g_state.clients[i].pending_files) {
            for (int j = 0; j < g_state.clients[i].pending_file_capacity; j++) {
                if (g_state.clients[i].pending_files[j].buffer)
                    free(g_state.clients[i].pending_files[j].buffer);
            }
            free(g_state.clients[i].pending_files);
        }
    }
    free(g_state.clients);
    mg_mgr_free(&mgr);
    return 0;
}

// ==================== HTTP 事件处理函数 ====================

static void fn(struct mg_connection *c, int ev, void *ev_data, void *fn_data) {
    (void)fn_data;
    if (ev == MG_EV_ACCEPT) {
        MG_INFO(("新连接：%p", c));
    }
    else if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *)ev_data;
        
        MG_INFO(("HTTP 请求：%.*s %.*s", 
                 (int)hm->method.len, hm->method.ptr,
                 (int)hm->uri.len, hm->uri.ptr));

        if (mg_http_match_uri(hm, "/api")) {
            handle_api_request(c, hm);
        }
        else if (mg_http_match_uri(hm, "/upload")) {
            handle_upload(c, hm);
        }
        else {
            mg_http_reply(c, 404, "", "Not Found\n");
        }
    }
    else if (ev == MG_EV_CLOSE) {
        MG_INFO(("连接关闭：%p", c));
        // 只清空 conn 指针，保留会话状态供后续请求使用
        client_session_t *session = find_client(c);
        if (session) {
            session->conn = NULL;
        }
    }
    else if (ev == MG_EV_ERROR) {
        MG_ERROR(("错误：%s", (char *)ev_data));
    }
}

// ==================== 配置加载 ====================

static void load_config(const char *config_path) {
    size_t json_len = 0;
    char *json_buf = mg_file_read(&mg_fs_posix, config_path, &json_len);
    if (json_buf == NULL) {
        MG_INFO(("未找到配置文件 %s，使用默认配置", config_path));
        return;
    }

    MG_INFO(("加载配置文件：%s", config_path));

    struct mg_str json = mg_str_n(json_buf, json_len);
    double val;
    if (mg_json_get_num(json, "$.port", &val)) g_config.port = (int)val;
    if (mg_json_get_num(json, "$.log_level", &val)) g_config.log_level = (int)val;
    if (mg_json_get_num(json, "$.max_clients", &val)) g_config.max_clients = (int)val;
    if (mg_json_get_num(json, "$.heartbeat_timeout", &val)) g_config.heartbeat_timeout = (int)val;

    free(json_buf);

    MG_INFO(("配置：端口=%d, 日志级别=%d, 最大客户端=%d, 心跳超时=%d 秒",
             g_config.port, g_config.log_level, g_config.max_clients, g_config.heartbeat_timeout));
}

// ==================== /api 端点处理 ====================

static void handle_api_request(struct mg_connection *c, struct mg_http_message *hm) {
    // 只处理 POST 请求
    if (mg_vcasecmp(&hm->method, "POST") != 0) {
        mg_http_reply(c, 405, "", "Method Not Allowed\n");
        return;
    }

    // 提取请求体
    const char *body = hm->body.ptr;
    size_t body_len = hm->body.len;

    if (body_len == 0) {
        send_error(c, 12, "空请求体", "UNKNOWN");
        return;
    }

    MG_INFO(("请求体：%.*s", (int)body_len, body));

    struct mg_str msg = mg_str_n(body, body_len);

    // 解析 COMMAND
    char *cmd_str = mg_json_get_str(msg, "$.COMMAND");
    if (!cmd_str) {
        send_error(c, 12, "无效的 JSON 格式", "UNKNOWN");
        return;
    }

    MG_INFO(("命令：%s", cmd_str));

    // 解析 data 字段（原始 JSON 字符串）
    int data_len = 0;
    int data_off = mg_json_get(msg, "$.data", &data_len);
    const char *data_ptr = msg.ptr + data_off;

    // 分发处理
    if (strcmp(cmd_str, "SIGNUP_ACCEPT") == 0) {
        handle_signup_accept(c, data_ptr);
    }
    else if (strcmp(cmd_str, "SIGNUP_CHECK") == 0) {
        handle_signup_check(c, data_ptr);
    }
    else if (strcmp(cmd_str, "AUTH_REQUEST") == 0) {
        handle_auth_request(c, data_ptr);
    }
    else if (strcmp(cmd_str, "AUTH_RESPONSE") == 0) {
        handle_auth_response(c, data_ptr);
    }
    else if (strcmp(cmd_str, "HEARTBEAT_REQUEST") == 0) {
        handle_heartbeat_request(c, data_ptr);
    }
    else if (strcmp(cmd_str, "CONTACTS_REQUEST") == 0) {
        handle_contacts_request(c, data_ptr);
    }
    else if (strcmp(cmd_str, "CHAT_SEND") == 0) {
        handle_chat_send(c, data_ptr);
    }
    else if (strcmp(cmd_str, "FILE_TRANSFER_START") == 0) {
        handle_file_transfer_start(c, data_ptr);
    }
    else if (strcmp(cmd_str, "FILE_TRANSFER_CHUNK") == 0) {
        handle_file_transfer_chunk(c, data_ptr);
    }
    else if (strcmp(cmd_str, "FILE_TRANSFER_END") == 0) {
        handle_file_transfer_end(c, data_ptr);
    }
    else {
        send_error(c, 13, "未知命令", cmd_str);
    }
    
    free(cmd_str);
}

// ==================== /upload 端点处理 ====================

static void handle_upload(struct mg_connection *c, struct mg_http_message *hm) {
    // 处理 multipart/form-data 文件上传
    struct mg_str *content_type = mg_http_get_header(hm, "Content-Type");
    
    if (content_type && mg_strstr(*content_type, mg_str("multipart/form-data"))) {
        // multipart 上传
        struct mg_http_part part;
        size_t offset = 0;
        
        while ((offset = mg_http_next_multipart(hm->body, offset, &part)) > 0) {
            MG_INFO(("文件部分：name=%.*s, filename=%.*s, body_len=%zu",
                     (int)part.name.len, part.name.ptr,
                     (int)part.filename.len, part.filename.ptr,
                     part.body.len));
            
            // 生成文件 ID
            char file_id[64];
            snprintf(file_id, sizeof(file_id), "file_%ld", time(NULL));
            
            // 返回成功响应
            char resp[256];
            snprintf(resp, sizeof(resp), 
                     "{\"file_id\":\"%s\",\"status\":\"success\",\"msg\":\"文件上传成功\"}",
                     file_id);
            send_json(c, 200, "UPLOAD_COMPLETE", resp);
            return;
        }
    }
    
    // 处理 PUT /upload/xxx?chunk=N 分块上传
    if (mg_vcasecmp(&hm->method, "PUT") == 0) {
        // 解析 URL 获取 file_id 和 chunk
        char file_id[64] = {0};
        int chunk_index = 0;
        
        // 简单解析：/upload/xxx?chunk=N
        const char *path = hm->uri.ptr;
        const char *slash = strchr(path + 8, '/');
        if (slash) {
            const char *query = strchr(slash, '?');
            if (query) {
                size_t id_len = query - slash - 1;
                if (id_len < sizeof(file_id)) {
                    memcpy(file_id, slash + 1, id_len);
                }
                // 解析 chunk 参数
                struct mg_str qs = mg_str(query + 1);
                struct mg_str chunk_val = mg_http_var(qs, mg_str("chunk"));
                if (chunk_val.len > 0) {
                    chunk_index = atoi(chunk_val.ptr);
                }
            }
        }
        
        MG_INFO(("分块上传：file_id=%s, chunk=%d, len=%zu", file_id, chunk_index, hm->body.len));
        
        // 返回确认
        char resp[128];
        snprintf(resp, sizeof(resp),
                 "{\"chunk_index\":%d,\"status\":\"received\",\"next_chunk\":%d}",
                 chunk_index, chunk_index + 1);
        send_json(c, 200, "FILE_TRANSFER_CHUNK_ACK", resp);
        return;
    }
    
    send_error(c, 14, "不支持的上传方式", "UPLOAD");
}
