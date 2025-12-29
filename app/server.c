/*
 * Chat Server Application
 * 
 * Author: kanjore
 * Date: 2025
 */

#define _XOPEN_SOURCE 600
#include "netcomm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>

#define MAX_CLIENTS 100
#define BUFFER_SIZE 1024

/* Global variables */
static int server_running = 1;
static int server_fd = -1;
static server_config_t server_config;
static connection_t *clients = NULL;
static int client_count = 0;
static pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Function declarations */
void* client_handler(void* arg);
void handle_text_message(connection_t *client, const char *text, uint32_t sequence);
void handle_file_request(connection_t *client, const char *filename, uint64_t file_size, uint32_t sequence);
void handle_file_chunk(connection_t *client, int transfer_id, uint32_t chunk_num, const uint8_t *data, size_t length, uint32_t sequence);
void handle_file_list_request(connection_t *client, uint32_t sequence);
void handle_file_download_request(connection_t *client, const char *filename, uint32_t sequence);
void handle_ack(connection_t *client, uint32_t sequence);
void broadcast_message(connection_t *sender, const char *message);
void cleanup_client(connection_t *client);
void signal_handler(int sig);
char* get_file_list(void);

/* Signal handler for clean shutdown */
void signal_handler(int sig) {
    printf("\nShutting down server...\n");
    server_running = 0;
    
    /* Close all client connections */
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < client_count; i++) {
        if (clients[i].connected) {
            netcomm_disconnect(&clients[i]);
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    
    /* Close server socket */
    if (server_fd >= 0) {
        netcomm_close(server_fd);
    }
    
    netcomm_cleanup();
    exit(0);
}

/* Client handler thread */
void* client_handler(void* arg) {
    connection_t *client = (connection_t*)arg;
    packet_t packet;
    int client_index = -1;

    /* Find client index by socket_fd */
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < server_config.max_clients; i++) {
        if (clients[i].socket_fd == client->socket_fd && clients[i].connected) {
            client_index = i;
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);

    if (client_index == -1) {
        netcomm_disconnect(client);
        free(client);
        pthread_exit(NULL);
    }
    
    printf("Client connected from %s\n", inet_ntoa(client->addr.sin_addr));

    /* Send welcome message */
    netcomm_send_text(client, "Welcome to the chat server!");
    
    /* Main message loop */
    while (server_running && client->connected) {
        int result = netcomm_receive_packet(client, &packet);
        
        if (result == NETCOMM_TIMEOUT) {
            /* Connection timeout - client may have disconnected */
            printf("Client %s timed out\n", inet_ntoa(client->addr.sin_addr));
            break;
        } else if (result == NETCOMM_INVALID_PACKET || result == NETCOMM_CONNECTION_CLOSED) {
            printf("Client %s connection closed or invalid packet\n", inet_ntoa(client->addr.sin_addr));
            break;
        }
        
        /* Handle different message types */
        switch (packet.header.type) {
            case MSG_TYPE_TEXT:
                handle_text_message(client, (const char*)packet.data, packet.header.sequence);
                break;
                
            case MSG_TYPE_FILE_REQUEST:
                {
                    size_t filename_len = packet.header.length - sizeof(uint64_t);
                    char filename[256];
                    uint64_t file_size;
                    
                    memcpy(filename, packet.data, filename_len);
                    filename[filename_len] = '\0';
                    memcpy(&file_size, packet.data + filename_len, sizeof(uint64_t));
                    
                    handle_file_request(client, filename, file_size, packet.header.sequence);
                }
                break;
                
            case MSG_TYPE_FILE_CHUNK:
                {
                    int transfer_id;
                    uint32_t chunk_num;
                    size_t data_len = packet.header.length - sizeof(int) - sizeof(uint32_t);
                    const uint8_t *data_ptr;
                    
                    memcpy(&transfer_id, packet.data, sizeof(int));
                    memcpy(&chunk_num, packet.data + sizeof(int), sizeof(uint32_t));
                    data_ptr = packet.data + sizeof(int) + sizeof(uint32_t);
                    
                    handle_file_chunk(client, transfer_id, chunk_num, data_ptr, data_len, packet.header.sequence);
                }
                break;
                
            case MSG_TYPE_ACK:
                {
                    uint32_t sequence;
                    memcpy(&sequence, packet.data, sizeof(uint32_t));
                    handle_ack(client, sequence);
                }
                break;
                
            case MSG_TYPE_DISCONNECT:
                printf("Client %s disconnected\n", inet_ntoa(client->addr.sin_addr));
                break;
                
            case MSG_TYPE_HEARTBEAT:
                /* Client sent heartbeat, acknowledge it */
                netcomm_send_ack(client, packet.header.sequence);
                break;

            case MSG_TYPE_FILE_LIST_REQUEST:
                handle_file_list_request(client, packet.header.sequence);
                break;

            case MSG_TYPE_FILE_DOWNLOAD_REQUEST:
                handle_file_download_request(client, (const char*)packet.data, packet.header.sequence);
                break;

            default:
                printf("Unknown message type: 0x%02X\n", packet.header.type);
                break;
        }
    }
    
    /* Clean up client */
    cleanup_client(client);
    free(client);
    pthread_exit(NULL);
}

/* Handle text messages */
void handle_text_message(connection_t *client, const char *text, uint32_t sequence) {
    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client->addr.sin_addr, ip_str, sizeof(ip_str));
    printf("Message from %s: %s\n", ip_str, text);

    /* Broadcast to all other clients */
    broadcast_message(client, text);

    /* Send acknowledgment */
    netcomm_send_ack(client, sequence);
}

/* Handle file transfer requests */
void handle_file_request(connection_t *client, const char *filename, uint64_t file_size, uint32_t sequence) {
    printf("File request from %s: %s (%llu bytes)\n",
           inet_ntoa(client->addr.sin_addr), filename, (unsigned long long)file_size);

    /* Create uploads directory if it doesn't exist */
    struct stat st = {0};
    if (stat(server_config.uploads_dir, &st) == -1) {
        mkdir(server_config.uploads_dir, 0755);
    }

    /* Send acknowledgment */
    netcomm_send_ack(client, sequence);
}

/* Handle file chunks */
void handle_file_chunk(connection_t *client, int transfer_id, uint32_t chunk_num, const uint8_t *data, size_t length, uint32_t sequence) {
    static FILE *current_file = NULL;
    static char current_filename[256] = {0};
    static uint64_t bytes_received = 0;

    /* If this is a new file, open it */
    if (strcmp(current_filename, "temp_file") != 0) {
        if (current_file) {
            fclose(current_file);
        }
        snprintf(current_filename, sizeof(current_filename), "%s/temp_file", server_config.uploads_dir);
        current_file = fopen(current_filename, "wb");
        bytes_received = 0;
    }

    /* Write chunk to file */
    if (current_file) {
        fwrite(data, 1, length, current_file);
        bytes_received += length;

        /* Send acknowledgment */
        netcomm_send_ack(client, sequence);

        printf("Received chunk %u from %s (%zu bytes, total: %llu)\n",
               chunk_num, inet_ntoa(client->addr.sin_addr), length, (unsigned long long)bytes_received);
    }
}

/* Handle acknowledgments */
void handle_ack(connection_t *client, uint32_t sequence) {
    printf("ACK received from %s for sequence %u\n", 
           inet_ntoa(client->addr.sin_addr), sequence);
}

/* Broadcast message to all clients */
void broadcast_message(connection_t *sender, const char *message) {
    pthread_mutex_lock(&clients_mutex);

    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &sender->addr.sin_addr, ip_str, sizeof(ip_str));

    char broadcast_msg[512];
    memset(broadcast_msg, 0, sizeof(broadcast_msg));
    snprintf(broadcast_msg, sizeof(broadcast_msg) - 1, "[%s]: %s", ip_str, message);

    for (int i = 0; i < client_count; i++) {
        if (clients[i].connected && clients[i].socket_fd != sender->socket_fd) {
            netcomm_send_text(&clients[i], broadcast_msg);
        }
    }

    pthread_mutex_unlock(&clients_mutex);
}

/* Handle file list request */
void handle_file_list_request(connection_t *client, uint32_t sequence) {
    printf("File list request from %s\n", inet_ntoa(client->addr.sin_addr));

    char *file_list = get_file_list();
    if (file_list) {
        netcomm_send_file_list_response(client, file_list);
        free(file_list);
    } else {
        netcomm_send_file_list_response(client, "No files available");
    }

    /* Send acknowledgment */
    netcomm_send_ack(client, sequence);
}

/* Handle file download request */
void handle_file_download_request(connection_t *client, const char *filename, uint32_t sequence) {
    printf("File download request from %s: %s\n", inet_ntoa(client->addr.sin_addr), filename);

    /* Check if file exists */
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/%s", server_config.uploads_dir, filename);

    FILE *file = fopen(filepath, "rb");
    if (!file) {
        printf("File not found: %s\n", filepath);
        netcomm_send_text(client, "ERROR: File not found");
        netcomm_send_ack(client, sequence);
        return;
    }

    /* Get file size */
    fseek(file, 0, SEEK_END);
    uint64_t file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    printf("Sending file %s (%llu bytes) to %s\n", filename, (unsigned long long)file_size, inet_ntoa(client->addr.sin_addr));

    /* Send file in chunks */
    uint32_t chunk_size = 1024;
    uint32_t chunk_num = 0;
    uint8_t buffer[1024];
    size_t bytes_read;

    while ((bytes_read = fread(buffer, 1, chunk_size, file)) > 0) {
        netcomm_send_file_chunk(client, 2, chunk_num++, buffer, bytes_read);

        /* Small delay to prevent overwhelming the network */
        usleep(1000);
    }

    fclose(file);
    printf("File transfer complete: %s\n", filename);

    /* Send acknowledgment */
    netcomm_send_ack(client, sequence);
}

/* Get list of available files */
char* get_file_list(void) {
    DIR *dir;
    struct dirent *ent;
    static char file_list[4096];
    memset(file_list, 0, sizeof(file_list));

    dir = opendir(server_config.uploads_dir);
    if (!dir) {
        return NULL;
    }

    int first = 1;
    while ((ent = readdir(dir)) != NULL) {
        /* Skip . and .. */
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }

        /* Skip directories */
        char filepath[512];
        snprintf(filepath, sizeof(filepath), "%s/%s", server_config.uploads_dir, ent->d_name);
        struct stat st;
        if (stat(filepath, &st) == 0 && S_ISDIR(st.st_mode)) {
            continue;
        }

        if (!first) {
            strncat(file_list, "\n", sizeof(file_list) - strlen(file_list) - 1);
        }
        strncat(file_list, ent->d_name, sizeof(file_list) - strlen(file_list) - 1);
        first = 0;
    }

    closedir(dir);

    if (strlen(file_list) == 0) {
        return NULL;
    }

    return strdup(file_list);
}

/* Clean up client connection */
void cleanup_client(connection_t *client) {
    pthread_mutex_lock(&clients_mutex);

    /* Remove client from array by socket_fd */
    for (int i = 0; i < client_count; i++) {
        if (clients[i].socket_fd == client->socket_fd) {
            /* Shift remaining clients down */
            for (int j = i; j < client_count - 1; j++) {
                clients[j] = clients[j + 1];
            }
            client_count--;
            break;
        }
    }

    pthread_mutex_unlock(&clients_mutex);

    netcomm_disconnect(client);
}

/* Main server function */
int main(int argc, char *argv[]) {
    /* Load server configuration */
    if (load_server_config(&server_config) != NETCOMM_SUCCESS) {
        fprintf(stderr, "Failed to load server configuration\n");
        return 1;
    }

    /* Parse command line arguments to override config */
    if (argc > 1) {
        server_config.listen_port = atoi(argv[1]);
    }

    /* Allocate clients array */
    clients = (connection_t*)malloc(sizeof(connection_t) * server_config.max_clients);
    if (!clients) {
        fprintf(stderr, "Failed to allocate memory for clients\n");
        return 1;
    }
    memset(clients, 0, sizeof(connection_t) * server_config.max_clients);

    /* Set up signal handler */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* Initialize networking library */
    if (netcomm_init() != NETCOMM_SUCCESS) {
        fprintf(stderr, "Failed to initialize networking library\n");
        free(clients);
        return 1;
    }

    /* Create server socket */
    server_fd = netcomm_listen(server_config.listen_port);
    if (server_fd < 0) {
        fprintf(stderr, "Failed to create server socket\n");
        netcomm_cleanup();
        free(clients);
        return 1;
    }

    printf("Chat server started on port %d\n", server_config.listen_port);
    printf("Press Ctrl+C to stop\n");

    /* Main accept loop */
    while (server_running) {
        /* Check if we have space for more clients */
        pthread_mutex_lock(&clients_mutex);
        if (client_count >= server_config.max_clients) {
            pthread_mutex_unlock(&clients_mutex);
            usleep(100000); /* Wait 100ms */
            continue;
        }
        pthread_mutex_unlock(&clients_mutex);

        /* Accept new connection */
        connection_t *client = (connection_t*)malloc(sizeof(connection_t));
        if (!client) {
            usleep(100000);
            continue;
        }

        int result = netcomm_accept(server_fd, client);
        if (result != NETCOMM_SUCCESS) {
            free(client);
            usleep(100000);
            continue;
        }

        /* Add client to array */
        pthread_mutex_lock(&clients_mutex);
        clients[client_count++] = *client;
        pthread_mutex_unlock(&clients_mutex);

        /* Create handler thread */
        pthread_t thread_id;
        if (pthread_create(&thread_id, NULL, client_handler, client) != 0) {
            fprintf(stderr, "Failed to create client handler thread\n");
            netcomm_disconnect(client);
            free(client);
        } else {
            pthread_detach(thread_id);
        }
    }

    /* Save configuration on exit */
    save_server_config(&server_config);

    /* Clean up */
    if (server_fd >= 0) {
        netcomm_close(server_fd);
    }
    if (clients) {
        free(clients);
    }
    netcomm_cleanup();

    return 0;
}
