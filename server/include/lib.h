#ifndef LIB_H
#define LIB_H

#include "mongoose.h"

// 客户端连接状态
typedef enum {
    CLIENT_STATE_INIT,
    CLIENT_STATE_SIGNUP,
    CLIENT_STATE_AUTHED,
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
    int received;
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
    char chat_token[64];
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
} server_config_t;

// 全局状态
typedef struct {
    client_session_t *clients;
    int next_client_id;
} server_state_t;

// 全局变量声明
extern server_state_t g_state;
extern server_config_t g_config;

// 辅助函数
client_session_t *find_client(struct mg_connection *c);
client_session_t *create_client(struct mg_connection *c);
void generate_token(char *buf, size_t len);
unsigned char *decompress_data(const unsigned char *input, size_t input_len, size_t *output_len);

// 发送响应
void send_json(struct mg_connection *c, int status, const char *command, const char *data);
void send_error(struct mg_connection *c, int code, const char *msg, const char *cmd);

// 命令处理函数
void handle_signup_accept(struct mg_connection *c, const char *data_str);
void handle_signup_check(struct mg_connection *c, const char *data_str);
void handle_auth_request(struct mg_connection *c, const char *data_str);
void handle_auth_response(struct mg_connection *c, const char *data_str);
void handle_heartbeat_request(struct mg_connection *c, const char *data_str);
void handle_contacts_request(struct mg_connection *c, const char *data_str);
void handle_chat_send(struct mg_connection *c, const char *data_str);
void handle_file_transfer_start(struct mg_connection *c, const char *data_str);
void handle_file_transfer_chunk(struct mg_connection *c, const char *data_str);
void handle_file_transfer_end(struct mg_connection *c, const char *data_str);

// 动态数组管理
void resize_contacts(client_session_t *session, int new_capacity);
void resize_pending_files(client_session_t *session, int new_capacity);
file_transfer_t *find_or_create_transfer(client_session_t *session, const char *file_id);
file_transfer_t *find_transfer(client_session_t *session, const char *file_id);
void free_transfer(file_transfer_t *ft);

// 解析 data 字符串（去掉 JSON 引号和转义）
char *parse_data_string(const char *data_str, size_t data_len);

#endif // LIB_H
