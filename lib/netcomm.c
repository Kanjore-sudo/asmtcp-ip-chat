   /*
    * Custom Network Communication Library Implementation
    * networking with Assembly optimizations
    * 
    * Author: kanjore
    * Date: 2025
    */

   #include "netcomm.h"
   #include <stdio.h>
   #include <stdlib.h>
   #include <string.h>
   #include <unistd.h>
   #include <errno.h>
   #include <fcntl.h>
   #include <time.h>
   #include <sys/time.h>
   #include <sys/stat.h>
   #include <signal.h>
   #include <sys/socket.h>
   #include <netinet/in.h>
   #include <arpa/inet.h>
   #include <sys/types.h>
   #include <sys/wait.h>

   /* Global variables */
   static int initialized = 0;
   static file_transfer_t transfers[100]; /* Max 100 concurrent transfers */
   static int transfer_count = 0;

   /* CRC32 Table for checksum calculation */
   static uint32_t crc32_table[256];

   /* Initialize CRC32 table */
   static void init_crc32_table(void) {
       uint32_t polynomial = 0xEDB88320;
       for (uint32_t i = 0; i < 256; i++) {
           uint32_t c = i;
           for (int j = 0; j < 8; j++) {
               if (c & 1) {
                   c = polynomial ^ (c >> 1);
               } else {
                   c >>= 1;
               }
           }
           crc32_table[i] = c;
       }
   }

   /* Initialize the networking library */
   int netcomm_init(void) {
       if (initialized) {
           return NETCOMM_SUCCESS;
       }
       
       /* Initialize CRC32 table */
       init_crc32_table();
       
       /* Initialize transfer tracking */
       memset(transfers, 0, sizeof(transfers));
       transfer_count = 0;
       
       /* Set up signal handling for clean shutdown */
       signal(SIGPIPE, SIG_IGN);
       
       initialized = 1;
       return NETCOMM_SUCCESS;
   }

   /* Clean up the networking library */
   int netcomm_cleanup(void) {
       if (!initialized) {
           return NETCOMM_SUCCESS;
       }
       
       /* Clean up any active transfers */
       for (int i = 0; i < transfer_count; i++) {
           if (transfers[i].active) {
               transfers[i].active = 0;
           }
       }
       transfer_count = 0;
       
       initialized = 0;
       return NETCOMM_SUCCESS;
   }

   /* Get current timestamp in microseconds */
   uint64_t get_timestamp(void) {
       struct timeval tv;
       gettimeofday(&tv, NULL);
       return (uint64_t)tv.tv_sec * 1000000 + (uint64_t)tv.tv_usec;
   }

   /* Set socket timeout */
   int set_socket_timeout(int sockfd, int seconds) {
       struct timeval timeout;
       timeout.tv_sec = seconds;
       timeout.tv_usec = 0;
       
       if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
           return NETCOMM_ERROR;
       }
       
       if (setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0) {
           return NETCOMM_ERROR;
       }
       
       return NETCOMM_SUCCESS;
   }

   /* Create a new connection context */
   static connection_t* create_connection(void) {
       connection_t *conn = (connection_t*)malloc(sizeof(connection_t));
       if (!conn) {
           return NULL;
       }
       
       memset(conn, 0, sizeof(connection_t));
       pthread_mutex_init(&conn->lock, NULL);
       conn->sequence_num = 1;
       conn->connected = 0;
       
       return conn;
   }

   /* Connect to a server */
   connection_t* netcomm_connect(const char *ip, int port) {
       if (!initialized) {
           netcomm_init();
       }
       
       connection_t *conn = create_connection();
       if (!conn) {
           return NULL;
       }
       
       conn->socket_fd = socket(AF_INET, SOCK_STREAM, 0);
       if (conn->socket_fd < 0) {
           free(conn);
           return NULL;
       }
       
       /* Set up address structure */
       conn->addr.sin_family = AF_INET;
       conn->addr.sin_port = htons(port);
       if (inet_pton(AF_INET, ip, &conn->addr.sin_addr) <= 0) {
           close(conn->socket_fd);
           free(conn);
           return NULL;
       }
       conn->addr_len = sizeof(conn->addr);
       
       /* Set timeout - IMPORTANT: Use shorter timeout for better error handling */
       set_socket_timeout(conn->socket_fd, 5); /* 5 seconds instead of DEFAULT_TIMEOUT */
       
       /* Connect to server */
       if (connect(conn->socket_fd, (struct sockaddr*)&conn->addr, conn->addr_len) < 0) {
           close(conn->socket_fd);
           free(conn);
           return NULL;
       }
       
       conn->connected = 1;
       return conn;
   }

   /* Create a listening socket */
   int netcomm_listen(int port) {
       if (!initialized) {
           netcomm_init();
       }
       
       int sockfd = socket(AF_INET, SOCK_STREAM, 0);
       if (sockfd < 0) {
           return NETCOMM_ERROR;
       }
       
       /* Set up address structure */
       struct sockaddr_in addr;
       addr.sin_family = AF_INET;
       addr.sin_addr.s_addr = INADDR_ANY;
       addr.sin_port = htons(port);
       
       /* Set socket options */
       int opt = 1;
       if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
           close(sockfd);
           return NETCOMM_ERROR;
       }
       
       /* Bind to address */
       if (bind(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
           close(sockfd);
           return NETCOMM_ERROR;
       }
       
       /* Listen for connections */
       if (listen(sockfd, 10) < 0) {
           close(sockfd);
           return NETCOMM_ERROR;
       }
       
       /* Set timeout */
       set_socket_timeout(sockfd, DEFAULT_TIMEOUT);
       
       return sockfd;
   }

   /* Accept a connection */
   int netcomm_accept(int server_fd, connection_t *client) {
       if (!client) {
           return NETCOMM_ERROR;
       }
       
       client->addr_len = sizeof(client->addr);
       int conn_fd = accept(server_fd, (struct sockaddr*)&client->addr, &client->addr_len);
       
       if (conn_fd < 0) {
           return NETCOMM_ERROR;
       }
       
       client->socket_fd = conn_fd;
       client->connected = 1;
       client->sequence_num = 1;
   
       /* Disable timeout on server side for blocking recv */
       struct timeval timeout;
       timeout.tv_sec = 0;
       timeout.tv_usec = 0;
       setsockopt(client->socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
       setsockopt(client->socket_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
   
       return NETCOMM_SUCCESS;
   }

   /* Disconnect a connection */
   int netcomm_disconnect(connection_t *conn) {
       if (!conn || !conn->connected) {
           return NETCOMM_SUCCESS;
       }
       
       pthread_mutex_lock(&conn->lock);
       if (conn->socket_fd >= 0) {
           close(conn->socket_fd);
           conn->socket_fd = -1;
       }
       conn->connected = 0;
       pthread_mutex_unlock(&conn->lock);
       
       return NETCOMM_SUCCESS;
   }

   /* Close a listening socket */
   int netcomm_close(int server_fd) {
       if (server_fd >= 0) {
           close(server_fd);
           return NETCOMM_SUCCESS;
       }
       return NETCOMM_ERROR;
   }

   /* Send a packet */
   int netcomm_send_packet(connection_t *conn, packet_t *packet) {
       if (!conn || !conn->connected || !packet) {
           return NETCOMM_ERROR;
       }
   
       pthread_mutex_lock(&conn->lock);
       
       /* Calculate checksum if not already calculated */
       if (packet->header.checksum == 0) {
           packet->header.checksum = calculate_checksum_asm(
               (uint8_t*)&packet->header,
               sizeof(packet_header_t) + packet->header.length
           );
       }
   
       /* Send the packet */
       size_t total_bytes = sizeof(packet_header_t) + packet->header.length;
       size_t total_sent = 0;
       
       while (total_sent < total_bytes) {
           ssize_t sent = send(conn->socket_fd, (uint8_t*)packet + total_sent,
                              total_bytes - total_sent, 0);
           
           if (sent < 0) {
               printf("DEBUG: send() failed with errno %d (%s)\n", errno, strerror(errno));
               pthread_mutex_unlock(&conn->lock);
               conn->connected = 0;  /* Mark connection as failed */
               return NETCOMM_ERROR;
           }
           
           total_sent += sent;
       }
       
       //printf("DEBUG: send() sent %zu bytes (expected %zu)\n", total_sent, total_bytes);
   
       conn->sequence_num++;
       pthread_mutex_unlock(&conn->lock);
   
       return NETCOMM_SUCCESS;
   }

   /* Receive a packet - FIXED VERSION */
   int netcomm_receive_packet(connection_t *conn, packet_t *packet) {
       if (!conn || !conn->connected || !packet) {
           return NETCOMM_ERROR;
       }
   
       pthread_mutex_lock(&conn->lock);
   
       /* Receive header first */
       size_t total_received = 0;
       while (total_received < sizeof(packet_header_t)) {
           ssize_t received = recv(conn->socket_fd, (uint8_t*)packet + total_received,
                                 sizeof(packet_header_t) - total_received, 0);
   
           //printf("DEBUG: recv() returned %zd bytes (header part), errno=%d (%s)\n", 
                  //received, errno, strerror(errno));
   
           if (received < 0) {
               if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                   /* Timeout or interrupted, connection still valid */
                   //printf("DEBUG: Timeout/interrupt on header recv, retrying...\n");
                   pthread_mutex_unlock(&conn->lock);
                   return NETCOMM_TIMEOUT;
               } else {
                   /* Other error, connection closed */
                   printf("DEBUG: recv() error: %s\n", strerror(errno));
                   pthread_mutex_unlock(&conn->lock);
                   conn->connected = 0;
                   return NETCOMM_CONNECTION_CLOSED;
               }
           }
   
           if (received == 0) {
               /* Connection closed by peer */
               printf("DEBUG: Connection closed by peer (EOF on header)\n");
               pthread_mutex_unlock(&conn->lock);
               conn->connected = 0;
               return NETCOMM_CONNECTION_CLOSED;
           }
   
           total_received += received;
       }
   
       if (total_received != sizeof(packet_header_t)) {
           printf("DEBUG: Incomplete header received: %zu bytes (expected %zu)\n", 
                  total_received, sizeof(packet_header_t));
           pthread_mutex_unlock(&conn->lock);
           return NETCOMM_INVALID_PACKET;
       }
   
       /* Validate packet header */
       if (!validate_packet_header_asm(&packet->header)) {
           printf("DEBUG: Packet header validation failed\n");
           print_packet_info(packet);
           pthread_mutex_unlock(&conn->lock);
           return NETCOMM_INVALID_PACKET;
       }
   
       /* Receive data */
       total_received = 0;
       while (total_received < packet->header.length) {
           ssize_t received = recv(conn->socket_fd, packet->data + total_received,
                                 packet->header.length - total_received, 0);
   
           //printf("DEBUG: recv() returned %zd bytes (data part), errno=%d (%s)\n",
                  //received, errno, strerror(errno));
   
           if (received < 0) {
               if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                   /* Timeout or interrupted, connection still valid */
                   //printf("DEBUG: Timeout/interrupt on data recv, retrying...\n");
                   pthread_mutex_unlock(&conn->lock);
                   return NETCOMM_TIMEOUT;
               } else {
                   /* Other error, connection closed */
                   printf("DEBUG: recv() error on data: %s\n", strerror(errno));
                   pthread_mutex_unlock(&conn->lock);
                   conn->connected = 0;
                   return NETCOMM_CONNECTION_CLOSED;
               }
           }
   
           if (received == 0) {
               /* Connection closed by peer */
               printf("DEBUG: Connection closed by peer (EOF on data)\n");
               pthread_mutex_unlock(&conn->lock);
               conn->connected = 0;
               return NETCOMM_CONNECTION_CLOSED;
           }
   
           total_received += received;
       }
   
       if (total_received != packet->header.length) {
           printf("DEBUG: Incomplete data received: %zu bytes (expected %u)\n",
                  total_received, packet->header.length);
           pthread_mutex_unlock(&conn->lock);
           return NETCOMM_INVALID_PACKET;
       }

       /* Null-terminate the data for safety (for text messages) */
       if (packet->header.length < sizeof(packet->data)) {
           packet->data[packet->header.length] = '\0';
       }

       /* Verify checksum */
       uint32_t received_checksum = packet->header.checksum;
       packet->header.checksum = 0;
   
       uint32_t calculated_checksum = calculate_checksum_asm(
           (uint8_t*)&packet->header,
           sizeof(packet_header_t) + packet->header.length
       );
   
       packet->header.checksum = received_checksum;
   
       if (received_checksum != calculated_checksum) {
           printf("DEBUG: Checksum mismatch - received: 0x%08X, calculated: 0x%08X\n", 
                  received_checksum, calculated_checksum);
           print_packet_info(packet);
           pthread_mutex_unlock(&conn->lock);
           return NETCOMM_INVALID_PACKET;
       }
   
       pthread_mutex_unlock(&conn->lock);
       return NETCOMM_SUCCESS;
   }

/* Send text message */
int netcomm_send_text(connection_t *conn, const char *text) {
    if (!conn || !text) {
        return NETCOMM_ERROR;
    }

    packet_t packet;
    size_t text_len = strlen(text);

    /* Initialize packet data to zero */
    memset(packet.data, 0, sizeof(packet.data));

    /* Create packet header */
    create_packet_header_asm(&packet.header, MSG_TYPE_TEXT, conn->sequence_num, text_len);

    /* Copy text data */
    memcpy(packet.data, text, text_len);

    int result = netcomm_send_packet(conn, &packet);

    /* If sending fails, mark connection as disconnected */
    if (result != NETCOMM_SUCCESS) {
        conn->connected = 0;
    }

    return result;
}

/* Send file request */
int netcomm_send_file_request(connection_t *conn, const char *filename, uint64_t file_size) {
    if (!conn || !filename) {
        return NETCOMM_ERROR;
    }

    packet_t packet;
    size_t filename_len = strlen(filename);

    /* Initialize packet data to zero */
    memset(packet.data, 0, sizeof(packet.data));

    /* Create packet header */
    create_packet_header_asm(&packet.header, MSG_TYPE_FILE_REQUEST, conn->sequence_num,
                          filename_len + sizeof(uint64_t));

    /* Copy filename and file size */
    memcpy(packet.data, filename, filename_len);
    memcpy(packet.data + filename_len, &file_size, sizeof(uint64_t));

    return netcomm_send_packet(conn, &packet);
}

/* Send file chunk */
int netcomm_send_file_chunk(connection_t *conn, int transfer_id, uint32_t chunk_num,
                          const uint8_t *data, size_t length) {
    if (!conn || !data || length == 0) {
        return NETCOMM_ERROR;
    }

    packet_t packet;

    /* Initialize packet data to zero */
    memset(packet.data, 0, sizeof(packet.data));

    /* Create packet header */
    create_packet_header_asm(&packet.header, MSG_TYPE_FILE_CHUNK, conn->sequence_num,
                          sizeof(int) + sizeof(uint32_t) + length);

    /* Copy transfer ID, chunk number, and data */
    memcpy(packet.data, &transfer_id, sizeof(int));
    memcpy(packet.data + sizeof(int), &chunk_num, sizeof(uint32_t));
    memcpy(packet.data + sizeof(int) + sizeof(uint32_t), data, length);

    return netcomm_send_packet(conn, &packet);
}

/* Send acknowledgment */
int netcomm_send_ack(connection_t *conn, uint32_t sequence) {
    if (!conn) {
        return NETCOMM_ERROR;
    }

    packet_t packet;

    /* Initialize packet data to zero */
    memset(packet.data, 0, sizeof(packet.data));

    /* Create packet header */
    create_packet_header_asm(&packet.header, MSG_TYPE_ACK, conn->sequence_num, sizeof(uint32_t));

    /* Copy sequence number */
    memcpy(packet.data, &sequence, sizeof(uint32_t));

    return netcomm_send_packet(conn, &packet);
}

/* Send heartbeat */
int netcomm_send_heartbeat(connection_t *conn) {
    if (!conn) {
        return NETCOMM_ERROR;
    }

    packet_t packet;

    /* Initialize packet data to zero */
    memset(packet.data, 0, sizeof(packet.data));

    /* Create packet header */
    create_packet_header_asm(&packet.header, MSG_TYPE_HEARTBEAT, conn->sequence_num, 0);

    return netcomm_send_packet(conn, &packet);
}

/* Send file list request */
int netcomm_send_file_list_request(connection_t *conn) {
    if (!conn) {
        return NETCOMM_ERROR;
    }

    packet_t packet;

    /* Initialize packet data to zero */
    memset(packet.data, 0, sizeof(packet.data));

    /* Create packet header */
    create_packet_header_asm(&packet.header, MSG_TYPE_FILE_LIST_REQUEST, conn->sequence_num, 0);

    return netcomm_send_packet(conn, &packet);
}

/* Send file list response */
int netcomm_send_file_list_response(connection_t *conn, const char *file_list) {
    if (!conn || !file_list) {
        return NETCOMM_ERROR;
    }

    packet_t packet;
    size_t list_len = strlen(file_list);

    /* Initialize packet data to zero */
    memset(packet.data, 0, sizeof(packet.data));

    /* Create packet header */
    create_packet_header_asm(&packet.header, MSG_TYPE_FILE_LIST_RESPONSE, conn->sequence_num, list_len);

    /* Copy file list */
    memcpy(packet.data, file_list, list_len);

    return netcomm_send_packet(conn, &packet);
}

/* Send file download request */
int netcomm_send_file_download_request(connection_t *conn, const char *filename) {
    if (!conn || !filename) {
        return NETCOMM_ERROR;
    }

    packet_t packet;
    size_t filename_len = strlen(filename);

    /* Initialize packet data to zero */
    memset(packet.data, 0, sizeof(packet.data));

    /* Create packet header */
    create_packet_header_asm(&packet.header, MSG_TYPE_FILE_DOWNLOAD_REQUEST, conn->sequence_num, filename_len);

    /* Copy filename */
    memcpy(packet.data, filename, filename_len);

    return netcomm_send_packet(conn, &packet);
}

/* Create configuration directory */
int create_config_directory(void) {
    char config_path[512];
    struct stat st = {0};

    /* Create .asmtcpip directory */
    if (stat(CONFIG_DIR, &st) == -1) {
        if (mkdir(CONFIG_DIR, 0755) != 0) {
            return NETCOMM_ERROR;
        }
    }

    /* Create subdirectories */
    snprintf(config_path, sizeof(config_path), "%s/%s", CONFIG_DIR, UPLOADS_DIR);
    if (stat(config_path, &st) == -1) {
        mkdir(config_path, 0755);
    }

    snprintf(config_path, sizeof(config_path), "%s/%s", CONFIG_DIR, DOWNLOADS_DIR);
    if (stat(config_path, &st) == -1) {
        mkdir(config_path, 0755);
    }

    snprintf(config_path, sizeof(config_path), "%s/%s", CONFIG_DIR, LOGS_DIR);
    if (stat(config_path, &st) == -1) {
        mkdir(config_path, 0755);
    }

    return NETCOMM_SUCCESS;
}

/* Load client configuration */
int load_client_config(client_config_t *config) {
    if (!config) {
        return NETCOMM_ERROR;
    }

    /* Set defaults */
    strcpy(config->server_ip, "127.0.0.1");
    config->server_port = DEFAULT_PORT;
    strcpy(config->username, "");
    snprintf(config->downloads_dir, sizeof(config->downloads_dir), "%s/%s", CONFIG_DIR, DOWNLOADS_DIR);
    config->auto_connect = 0;
    config->debug_mode = 0;

    /* Try to load from file */
    char config_path[512];
    snprintf(config_path, sizeof(config_path), "%s/%s", CONFIG_DIR, CLIENT_CONFIG_FILE);

    FILE *file = fopen(config_path, "r");
    if (!file) {
        /* File doesn't exist, save defaults */
        return save_client_config(config);
    }

    /* Read configuration */
    char line[256];
    while (fgets(line, sizeof(line), file)) {
        char *key = strtok(line, "=");
        char *value = strtok(NULL, "\n");

        if (!key || !value) continue;

        /* Remove leading/trailing whitespace */
        while (*key == ' ' || *key == '\t') key++;
        char *end = key + strlen(key) - 1;
        while (end > key && (*end == ' ' || *end == '\t')) end--;
        *(end + 1) = '\0';

        while (*value == ' ' || *value == '\t') value++;
        end = value + strlen(value) - 1;
        while (end > value && (*end == ' ' || *end == '\t')) end--;
        *(end + 1) = '\0';

        /* Parse values */
        if (strcmp(key, "server_ip") == 0) {
            strncpy(config->server_ip, value, sizeof(config->server_ip) - 1);
        } else if (strcmp(key, "server_port") == 0) {
            config->server_port = atoi(value);
        } else if (strcmp(key, "username") == 0) {
            strncpy(config->username, value, sizeof(config->username) - 1);
        } else if (strcmp(key, "downloads_dir") == 0) {
            strncpy(config->downloads_dir, value, sizeof(config->downloads_dir) - 1);
        } else if (strcmp(key, "auto_connect") == 0) {
            config->auto_connect = atoi(value);
        } else if (strcmp(key, "debug_mode") == 0) {
            config->debug_mode = atoi(value);
        }
    }

    fclose(file);
    return NETCOMM_SUCCESS;
}

/* Save client configuration */
int save_client_config(const client_config_t *config) {
    if (!config) {
        return NETCOMM_ERROR;
    }

    /* Ensure config directory exists */
    if (create_config_directory() != NETCOMM_SUCCESS) {
        return NETCOMM_ERROR;
    }

    char config_path[512];
    snprintf(config_path, sizeof(config_path), "%s/%s", CONFIG_DIR, CLIENT_CONFIG_FILE);

    FILE *file = fopen(config_path, "w");
    if (!file) {
        return NETCOMM_ERROR;
    }

    fprintf(file, "# ASMTCPIP Client Configuration\n");
    fprintf(file, "server_ip=%s\n", config->server_ip);
    fprintf(file, "server_port=%d\n", config->server_port);
    fprintf(file, "username=%s\n", config->username);
    fprintf(file, "downloads_dir=%s\n", config->downloads_dir);
    fprintf(file, "auto_connect=%d\n", config->auto_connect);
    fprintf(file, "debug_mode=%d\n", config->debug_mode);

    fclose(file);
    return NETCOMM_SUCCESS;
}

/* Load server configuration */
int load_server_config(server_config_t *config) {
    if (!config) {
        return NETCOMM_ERROR;
    }

    /* Set defaults */
    config->listen_port = DEFAULT_PORT;
    snprintf(config->uploads_dir, sizeof(config->uploads_dir), "%s/%s", CONFIG_DIR, UPLOADS_DIR);
    snprintf(config->logs_dir, sizeof(config->logs_dir), "%s/%s", CONFIG_DIR, LOGS_DIR);
    config->max_clients = 100;
    config->debug_mode = 0;
    config->enable_file_transfer = 1;

    /* Try to load from file */
    char config_path[512];
    snprintf(config_path, sizeof(config_path), "%s/%s", CONFIG_DIR, SERVER_CONFIG_FILE);

    FILE *file = fopen(config_path, "r");
    if (!file) {
        /* File doesn't exist, save defaults */
        return save_server_config(config);
    }

    /* Read configuration */
    char line[256];
    while (fgets(line, sizeof(line), file)) {
        char *key = strtok(line, "=");
        char *value = strtok(NULL, "\n");

        if (!key || !value) continue;

        /* Remove leading/trailing whitespace */
        while (*key == ' ' || *key == '\t') key++;
        char *end = key + strlen(key) - 1;
        while (end > key && (*end == ' ' || *end == '\t')) end--;
        *(end + 1) = '\0';

        while (*value == ' ' || *value == '\t') value++;
        end = value + strlen(value) - 1;
        while (end > value && (*end == ' ' || *end == '\t')) end--;
        *(end + 1) = '\0';

        /* Parse values */
        if (strcmp(key, "listen_port") == 0) {
            config->listen_port = atoi(value);
        } else if (strcmp(key, "uploads_dir") == 0) {
            strncpy(config->uploads_dir, value, sizeof(config->uploads_dir) - 1);
        } else if (strcmp(key, "logs_dir") == 0) {
            strncpy(config->logs_dir, value, sizeof(config->logs_dir) - 1);
        } else if (strcmp(key, "max_clients") == 0) {
            config->max_clients = atoi(value);
        } else if (strcmp(key, "debug_mode") == 0) {
            config->debug_mode = atoi(value);
        } else if (strcmp(key, "enable_file_transfer") == 0) {
            config->enable_file_transfer = atoi(value);
        }
    }

    fclose(file);
    return NETCOMM_SUCCESS;
}

/* Save server configuration */
int save_server_config(const server_config_t *config) {
    if (!config) {
        return NETCOMM_ERROR;
    }

    /* Ensure config directory exists */
    if (create_config_directory() != NETCOMM_SUCCESS) {
        return NETCOMM_ERROR;
    }

    char config_path[512];
    snprintf(config_path, sizeof(config_path), "%s/%s", CONFIG_DIR, SERVER_CONFIG_FILE);

    FILE *file = fopen(config_path, "w");
    if (!file) {
        return NETCOMM_ERROR;
    }

    fprintf(file, "# ASMTCPIP Server Configuration\n");
    fprintf(file, "listen_port=%d\n", config->listen_port);
    fprintf(file, "uploads_dir=%s\n", config->uploads_dir);
    fprintf(file, "logs_dir=%s\n", config->logs_dir);
    fprintf(file, "max_clients=%d\n", config->max_clients);
    fprintf(file, "debug_mode=%d\n", config->debug_mode);
    fprintf(file, "enable_file_transfer=%d\n", config->enable_file_transfer);

    fclose(file);
    return NETCOMM_SUCCESS;
}

/* Print packet information for debugging */
void print_packet_info(const packet_t *packet) {
    if (!packet) {
        return;
    }

    printf("Packet Info:\n");
    printf("  Magic: 0x%08X\n", packet->header.magic);
    printf("  Type: 0x%02X\n", packet->header.type);
    printf("  Sequence: %u\n", packet->header.sequence);
    printf("  Length: %u\n", packet->header.length);
    printf("  Checksum: 0x%08X\n", packet->header.checksum);
    printf("  Timestamp: %llu\n", (unsigned long long)packet->header.timestamp);
}
