/*
 * Chat Client Application
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
#include <sys/time.h>
#include <ncurses.h>

#define BUFFER_SIZE 1024
#define MAX_CHAT_MESSAGES 100
#define CHAT_DISPLAY_HEIGHT 15

/* Global variables */
static connection_t *server_conn = NULL;
static int client_running = 1;
static client_config_t client_config;
static pthread_mutex_t input_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t chat_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Chat interface */
typedef struct {
    char message[512];
    char timestamp[32];
} chat_message_t;

static chat_message_t chat_history[MAX_CHAT_MESSAGES];
static int chat_message_count = 0;
static int screen_needs_redraw = 1;

/* Ncurses windows */
WINDOW *chat_win;
WINDOW *input_win;

/* Function declarations */
void* receive_thread(void* arg);
void* input_thread(void* arg);
void* display_thread(void* arg);
void send_file(const char *filename);
void handle_server_message(const char *message);
void handle_file_chunk(int transfer_id, uint32_t chunk_num, const uint8_t *data, size_t length);
void add_chat_message(const char *message);
void clear_screen(void);
void draw_chat_interface(void);
void cleanup_client(void);
void signal_handler(int sig);

/* Signal handler for clean shutdown */
void signal_handler(int sig) {
    client_running = 0;
    endwin();
    printf("\nShutting down client...\n");
    if (server_conn) {
        netcomm_disconnect(server_conn);
    }
    netcomm_cleanup();
    exit(0);
}

/* Receive thread - handles incoming messages from server */
void* receive_thread(void* arg) {
    connection_t *conn = (connection_t*)arg;
    packet_t packet;
    
    while (client_running && conn->connected) {
        int result = netcomm_receive_packet(conn, &packet);
        
        if (result == NETCOMM_TIMEOUT) {
            /* Send heartbeat to keep connection alive */
            netcomm_send_heartbeat(conn);
            continue;
        } else if (result == NETCOMM_INVALID_PACKET || result == NETCOMM_CONNECTION_CLOSED) {
            printf("Connection lost!\n");
            client_running = 0;
            break;
        }
        
        /* Handle different message types */
        switch (packet.header.type) {
            case MSG_TYPE_TEXT:
                handle_server_message((const char*)packet.data);
                break;
                
            case MSG_TYPE_FILE_REQUEST:
                {
                    size_t filename_len = packet.header.length - sizeof(uint64_t);
                    char filename[256];
                    uint64_t file_size;

                    memcpy(filename, packet.data, filename_len);
                    filename[filename_len] = '\0';
                    memcpy(&file_size, packet.data + filename_len, sizeof(uint64_t));

                    printf("File transfer request: %s (%llu bytes)\n",
                           filename, (unsigned long long)file_size);
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

                    handle_file_chunk(transfer_id, chunk_num, data_ptr, data_len);
                }
                break;

            case MSG_TYPE_FILE_LIST_RESPONSE:
                printf("Available files:\n%s\n", (const char*)packet.data);
                break;
                
            case MSG_TYPE_ACK:
                {
                    uint32_t sequence;
                    memcpy(&sequence, packet.data, sizeof(uint32_t));
                    //printf("ACK received for sequence %u\n", sequence);
                }
                break;
                
            case MSG_TYPE_HEARTBEAT:
                /* Server sent heartbeat, acknowledge it */
                netcomm_send_ack(conn, packet.header.sequence);
                break;
                
            default:
                printf("Unknown message type: 0x%02X\n", packet.header.type);
                break;
        }
    }
    
    client_running = 0;
    pthread_exit(NULL);
}

/* Function to get a line of input, filtering out escape sequences */
int get_input_line(WINDOW *win, char *buffer, size_t size) {
    int pos = 0;
    int ch;

    /* Consume any buffered input */
    nodelay(win, TRUE);
    while (wgetch(win) != ERR);
    nodelay(win, FALSE);

    wmove(win, 0, 2); /* After "> " */
    wclrtoeol(win);
    wrefresh(win);

    while (pos < size - 1) {
        ch = wgetch(win);

        if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
            break;
        } else if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b') {
            if (pos > 0) {
                pos--;
                mvwdelch(win, 0, pos + 2);
                wrefresh(win);
            }
        } else if (ch >= 32 && ch <= 126) { /* Printable characters */
            buffer[pos++] = ch;
            waddch(win, ch);
            wrefresh(win);
        }
        /* Ignore escape sequences and other non-printable */
    }

    buffer[pos] = '\0';
    return pos;
}

/* Input thread - handles user input and display */
void* input_thread(void* arg) {
    char buffer[BUFFER_SIZE];

    /* Initial draw */
    draw_chat_interface();
    screen_needs_redraw = 0;

    while (client_running && server_conn && server_conn->connected) {
        /* Redraw if needed */
        if (screen_needs_redraw) {
            draw_chat_interface();
            screen_needs_redraw = 0;
        }

        /* Get input */
        get_input_line(input_win, buffer, sizeof(buffer));

        /* Process input */
        if (strlen(buffer) > 0) {
            /* Check for commands */
            if (strcmp(buffer, "/quit") == 0) {
                client_running = 0;
                break;
            } else if (strcmp(buffer, "/list") == 0) {
                netcomm_send_file_list_request(server_conn);
            } else if (strncmp(buffer, "/download ", 10) == 0) {
                char *filename = buffer + 10;
                while (*filename == ' ') filename++; /* Skip spaces */
                netcomm_send_file_download_request(server_conn, filename);
            } else if (strncmp(buffer, "/file ", 6) == 0) {
                char *filename = buffer + 6;
                while (*filename == ' ') filename++; /* Skip spaces */
                send_file(filename);
            } else {
                /* Send text message */
                int send_result = netcomm_send_text(server_conn, buffer);
                if (send_result == NETCOMM_SUCCESS) {
                    /* Add to local chat history */
                    char user_message[BUFFER_SIZE + 64];
                    snprintf(user_message, sizeof(user_message), "[%s]: %s", client_config.username, buffer);
                    add_chat_message(user_message);
                    /* Redraw to show the message */
                    draw_chat_interface();
                    screen_needs_redraw = 0;
                    /* Give time for server response */
                    usleep(100000); /* 100ms */
                }
            }
        }
    }

    pthread_exit(NULL);
}

/* Send file to server */
void send_file(const char *filename) {
    if (!filename || strlen(filename) == 0) {
        printf("Please specify a filename\n");
        return;
    }
    
    /* Open file */
    FILE *file = fopen(filename, "rb");
    if (!file) {
        printf("Error: Could not open file '%s'\n", filename);
        return;
    }
    
    /* Get file size */
    fseek(file, 0, SEEK_END);
    uint64_t file_size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    printf("Sending file: %s (%llu bytes)\n", filename, (unsigned long long)file_size);
    
    /* Send file request */
    netcomm_send_file_request(server_conn, filename, file_size);
    
    /* Send file in chunks */
    uint32_t chunk_size = 1024;
    uint32_t chunk_num = 0;
    uint8_t buffer[1024];
    size_t bytes_read;
    
    while ((bytes_read = fread(buffer, 1, chunk_size, file)) > 0) {
        netcomm_send_file_chunk(server_conn, 1, chunk_num++, buffer, bytes_read);
        
        /* Small delay to prevent overwhelming the network */
        usleep(1000);
    }
    
    fclose(file);
    printf("File transfer complete\n");
}

/* Clear screen using ANSI escape codes */
void clear_screen(void) {
    printf("\033[2J\033[H");
    fflush(stdout);
}

/* Draw the chat interface */
void draw_chat_interface(void) {
    wclear(chat_win);
    wclear(input_win);

    /* Header */
    mvwprintw(chat_win, 0, 0, "=== Chat Client - Connected as: %s ===", client_config.username);
    mvwprintw(chat_win, 1, 0, "Commands: /quit to exit, /file <filename> to send file");

    /* Horizontal line */
    int i;
    for (i = 0; i < COLS; i++) {
        mvwaddch(chat_win, 2, i, '-');
    }

    pthread_mutex_lock(&chat_mutex);

    /* Display chat messages */
    int start_idx = (chat_message_count > CHAT_DISPLAY_HEIGHT - 4) ?
                   chat_message_count - (CHAT_DISPLAY_HEIGHT - 4) : 0;

    int line = 3;
    for (int i = start_idx; i < chat_message_count && line < CHAT_DISPLAY_HEIGHT; i++) {
        mvwprintw(chat_win, line++, 0, "%s %s", chat_history[i].timestamp, chat_history[i].message);
    }

    /* Separator */
    for (i = 0; i < COLS; i++) {
        mvwaddch(chat_win, CHAT_DISPLAY_HEIGHT - 1, i, '-');
    }

    pthread_mutex_unlock(&chat_mutex);

    /* Draw input prompt */
    mvwprintw(input_win, 0, 0, "> ");

    wrefresh(chat_win);
    wrefresh(input_win);
}

/* Add message to chat history */
void add_chat_message(const char *message) {
    pthread_mutex_lock(&chat_mutex);

    if (chat_message_count >= MAX_CHAT_MESSAGES) {
        /* Shift messages up to make room */
        for (int i = 1; i < MAX_CHAT_MESSAGES; i++) {
            chat_history[i-1] = chat_history[i];
        }
        chat_message_count = MAX_CHAT_MESSAGES - 1;
    }

    /* Add timestamp */
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    strftime(chat_history[chat_message_count].timestamp,
             sizeof(chat_history[chat_message_count].timestamp),
             "[%H:%M:%S]", tm_info);

    /* Copy message */
    strncpy(chat_history[chat_message_count].message, message,
            sizeof(chat_history[chat_message_count].message) - 1);
    chat_history[chat_message_count].message[sizeof(chat_history[chat_message_count].message) - 1] = '\0';

    chat_message_count++;
    screen_needs_redraw = 1;

    pthread_mutex_unlock(&chat_mutex);
}

/* Display thread - handles interface updates */
void* display_thread(void* arg) {
    while (client_running) {
        if (screen_needs_redraw) {
            draw_chat_interface();
            screen_needs_redraw = 0;
        }
        usleep(100000); /* Check every 100ms */
    }
    pthread_exit(NULL);
}

/* Handle file chunks from server */
void handle_file_chunk(int transfer_id, uint32_t chunk_num, const uint8_t *data, size_t length) {
    static FILE *current_file = NULL;
    static char current_filename[256] = {0};
    static uint64_t bytes_received = 0;

    /* For downloads, transfer_id 2 is used by server */
    if (transfer_id == 2) {
        /* If this is the first chunk, create the file */
        if (chunk_num == 0) {
            if (current_file) {
                fclose(current_file);
            }

            /* Create filename based on timestamp to avoid conflicts */
            time_t now = time(NULL);
            snprintf(current_filename, sizeof(current_filename), "%s/downloaded_file_%ld", client_config.downloads_dir, now);
            current_file = fopen(current_filename, "wb");
            bytes_received = 0;

            if (!current_file) {
                printf("Error: Could not create download file\n");
                return;
            }

            printf("Starting file download...\n");
        }

        /* Write chunk to file */
        if (current_file) {
            fwrite(data, 1, length, current_file);
            bytes_received += length;

            /* If this is a small chunk or we detect end of transfer, close file */
            if (length < 1024 || chunk_num > 10000) {  /* Simple heuristic */
                fclose(current_file);
                current_file = NULL;
                printf("File download complete: %s (%llu bytes)\n", current_filename, (unsigned long long)bytes_received);
                bytes_received = 0;
                memset(current_filename, 0, sizeof(current_filename));
            }
        }
    }
}

/* Handle server messages */
void handle_server_message(const char *message) {
    add_chat_message(message);
}

/* Clean up client */
void cleanup_client(void) {
    endwin();
    printf("Goodbye!\n");

    if (server_conn) {
        netcomm_disconnect(server_conn);
        free(server_conn);
        server_conn = NULL;
    }
    netcomm_cleanup();
}

/* Main client function */
int main(int argc, char *argv[]) {
    /* Load client configuration */
    if (load_client_config(&client_config) != NETCOMM_SUCCESS) {
        fprintf(stderr, "Failed to load client configuration\n");
        return 1;
    }

    /* Parse command line arguments to override config */
    if (argc > 1) {
        strncpy(client_config.server_ip, argv[1], sizeof(client_config.server_ip) - 1);
    }
    if (argc > 2) {
        client_config.server_port = atoi(argv[2]);
    }
    if (argc > 3) {
        strncpy(client_config.username, argv[3], sizeof(client_config.username) - 1);
    } else if (strlen(client_config.username) == 0) {
        /* Get username interactively */
        printf("Enter your username: ");
        fflush(stdout);
        if (fgets(client_config.username, sizeof(client_config.username), stdin) == NULL) {
            fprintf(stderr, "Error reading username\n");
            return 1;
        }

        /* Remove newline from username */
        size_t len = strlen(client_config.username);
        if (len > 0 && client_config.username[len-1] == '\n') {
            client_config.username[len-1] = '\0';
        }

        if (strlen(client_config.username) == 0) {
            strcpy(client_config.username, "Anonymous");
        }

        /* Save updated configuration */
        save_client_config(&client_config);
    }

    /* Set up signal handler */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* Initialize networking library */
    if (netcomm_init() != NETCOMM_SUCCESS) {
        fprintf(stderr, "Failed to initialize networking library\n");
        return 1;
    }

    /* Connect to server */
    printf("Connecting to server at %s:%d...\n", client_config.server_ip, client_config.server_port);
    server_conn = netcomm_connect(client_config.server_ip, client_config.server_port);

    if (!server_conn) {
        fprintf(stderr, "Failed to connect to server\n");
        netcomm_cleanup();
        return 1;
    }

    printf("Connected to server!\n");
    sleep(1); /* Give user time to see connection message */

    /* Initialize ncurses */
    initscr();
    cbreak();
    noecho();
    chat_win = newwin(CHAT_DISPLAY_HEIGHT, COLS, 0, 0);
    input_win = newwin(1, COLS, CHAT_DISPLAY_HEIGHT, 0);
    keypad(input_win, TRUE);
    scrollok(chat_win, TRUE);
    wrefresh(chat_win);
    wrefresh(input_win);

    /* Create receive thread */
    pthread_t recv_thread;
    if (pthread_create(&recv_thread, NULL, receive_thread, server_conn) != 0) {
        fprintf(stderr, "Failed to create receive thread\n");
        cleanup_client();
        return 1;
    }

    /* Create input thread */
    pthread_t input_thread_id;
    if (pthread_create(&input_thread_id, NULL, input_thread, NULL) != 0) {
        fprintf(stderr, "Failed to create input thread\n");
        cleanup_client();
        return 1;
    }

    /* Create display thread */
    pthread_t display_thread_id;
    if (pthread_create(&display_thread_id, NULL, display_thread, NULL) != 0) {
        fprintf(stderr, "Failed to create display thread\n");
        cleanup_client();
        return 1;
    }

    /* Wait for threads to complete */
    pthread_join(recv_thread, NULL);
    pthread_join(input_thread_id, NULL);
    pthread_join(display_thread_id, NULL);

    /* Save configuration on exit */
    save_client_config(&client_config);

    /* Clean up */
    cleanup_client();

    return 0;
}
