/*
 * Custom Network Communication Library
 * networking with Assembly optimizations
 * 
 * Author: kanjore
 * Date: 2025
 */

#ifndef NETCOMM_H
#define NETCOMM_H

#include <stdint.h>
#include <stddef.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>

/* Protocol Constants */
#define PROTOCOL_MAGIC 0xDEADBEEF
#define MAX_PACKET_SIZE 65536
#define MAX_MESSAGE_SIZE 65000
#define MAX_FILENAME_LEN 256
#define MAX_USERNAME_LEN 64
#define DEFAULT_PORT 8080
#define DEFAULT_TIMEOUT 30

/* Message Types */
#define MSG_TYPE_TEXT 0x01
#define MSG_TYPE_FILE_REQUEST 0x02
#define MSG_TYPE_FILE_CHUNK 0x03
#define MSG_TYPE_ACK 0x04
#define MSG_TYPE_CONNECT 0x05
#define MSG_TYPE_DISCONNECT 0x06
#define MSG_TYPE_HEARTBEAT 0x07
#define MSG_TYPE_FILE_LIST_REQUEST 0x08
#define MSG_TYPE_FILE_LIST_RESPONSE 0x09
#define MSG_TYPE_FILE_DOWNLOAD_REQUEST 0x0A

/* Error Codes */
#define NETCOMM_SUCCESS 0
#define NETCOMM_ERROR -1
#define NETCOMM_TIMEOUT -2
#define NETCOMM_INVALID_PACKET -3
#define NETCOMM_CONNECTION_CLOSED -4
#define NETCOMM_MEMORY_ERROR -5

/* Configuration Constants */
#define CONFIG_DIR ".asmtcpip"
#define CLIENT_CONFIG_FILE "client.conf"
#define SERVER_CONFIG_FILE "server.conf"
#define UPLOADS_DIR "uploads"
#define DOWNLOADS_DIR "downloads"
#define LOGS_DIR "logs"

/* Client Configuration Structure */
typedef struct {
    char server_ip[64];
    int server_port;
    char username[MAX_USERNAME_LEN];
    char downloads_dir[MAX_FILENAME_LEN];
    int auto_connect;
    int debug_mode;
} client_config_t;

/* Server Configuration Structure */
typedef struct {
    int listen_port;
    char uploads_dir[MAX_FILENAME_LEN];
    char logs_dir[MAX_FILENAME_LEN];
    int max_clients;
    int debug_mode;
    int enable_file_transfer;
} server_config_t;

/* Assembly Function Declarations */
#ifdef __cplusplus
extern "C" {
#endif

/* Packet Header Structure */
typedef struct {
    uint32_t magic;         /* Protocol magic number */
    uint8_t type;           /* Message type */
    uint32_t sequence;      /* Packet sequence number */
    uint32_t length;        /* Data length */
    uint32_t checksum;      /* CRC32 checksum */
    uint64_t timestamp;     /* Packet timestamp */
    uint8_t reserved[7];    /* Reserved for future use */
} __attribute__((packed)) packet_header_t;

/* Message Structure */
typedef struct {
    packet_header_t header;
    uint8_t data[MAX_MESSAGE_SIZE];
} packet_t;

/* Connection Context */
typedef struct {
    int socket_fd;
    struct sockaddr_in addr;
    socklen_t addr_len;
    uint32_t sequence_num;
    pthread_mutex_t lock;
    int connected;
    char username[MAX_USERNAME_LEN];
} connection_t;

/* File Transfer Context */
typedef struct {
    char filename[MAX_FILENAME_LEN];
    uint64_t file_size;
    uint64_t bytes_sent;
    uint64_t bytes_received;
    uint32_t chunk_size;
    int transfer_id;
    int active;
} file_transfer_t;

/* Assembly-optimized functions */
extern uint32_t calculate_checksum_asm(const uint8_t *data, size_t length);
extern void create_packet_header_asm(packet_header_t *header, uint8_t type, uint32_t sequence, uint32_t length);
extern int validate_packet_header_asm(const packet_header_t *header);
extern void encrypt_data_asm(uint8_t *data, size_t length, uint32_t key);
extern void decrypt_data_asm(uint8_t *data, size_t length, uint32_t key);

/* Configuration functions */
int create_config_directory(void);
int load_client_config(client_config_t *config);
int save_client_config(const client_config_t *config);
int load_server_config(server_config_t *config);
int save_server_config(const server_config_t *config);

/* C library functions */
int netcomm_init(void);
int netcomm_cleanup(void);

/* Connection management */
connection_t* netcomm_connect(const char *ip, int port);
int netcomm_listen(int port);
int netcomm_accept(int server_fd, connection_t *client);
int netcomm_disconnect(connection_t *conn);
int netcomm_close(int server_fd);

/* Packet operations */
int netcomm_send_packet(connection_t *conn, packet_t *packet);
int netcomm_receive_packet(connection_t *conn, packet_t *packet);
int netcomm_send_text(connection_t *conn, const char *text);
int netcomm_send_file_request(connection_t *conn, const char *filename, uint64_t file_size);
int netcomm_send_file_chunk(connection_t *conn, int transfer_id, uint32_t chunk_num, const uint8_t *data, size_t length);
int netcomm_send_ack(connection_t *conn, uint32_t sequence);
int netcomm_send_heartbeat(connection_t *conn);
int netcomm_send_file_list_request(connection_t *conn);
int netcomm_send_file_list_response(connection_t *conn, const char *file_list);
int netcomm_send_file_download_request(connection_t *conn, const char *filename);

/* File transfer */
int netcomm_start_file_transfer(connection_t *conn, const char *filename, const char *local_path);
int netcomm_receive_file(connection_t *conn, const char *local_path);
int netcomm_get_transfer_progress(int transfer_id, float *progress);

/* Utility functions */
uint64_t get_timestamp(void);
int set_socket_timeout(int sockfd, int seconds);
void print_packet_info(const packet_t *packet);

#ifdef __cplusplus
}
#endif

#endif /* NETCOMM_H */
