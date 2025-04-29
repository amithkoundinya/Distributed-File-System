#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <dirent.h>
#include <libgen.h>
#include <errno.h>
#include <signal.h>

#define BUFFER_SIZE 4096
#define COMMAND_SIZE 1024
#define MAX_FILEPATH 1024
#define MAX_FILENAME 256
#define S1_PORT 8386
#define S2_PORT 8387
#define S3_PORT 8388
#define S4_PORT 8389
#define MAX_PENDING 10
#define S1_BASE_DIR "~/S1"

// Server information structure
typedef struct {
    char ip[16];
    int port;
} ServerInfo;

// Function prototypes
void process_client(int client_socket);
int create_directory_path(const char *path);
int connect_to_server(const char *server_ip, int port);
int handle_upload_command(char *command, int client_socket);
int handle_download_command(char *command, int client_socket);
int handle_remove_command(char *command, int client_socket);
int handle_download_tar_command(char *command, int client_socket);
int handle_display_filenames_command(char *command, int client_socket);
void transfer_file_to_server(const char *filename, const char *dest_path, int server_type);
int send_file_to_client(const char *filepath, int client_socket);
int receive_file_from_client(const char *filepath, int client_socket);
void expand_path(const char *path, char *expanded_path);
int is_path_in_s1(const char *path);
char* get_file_extension(const char *filename);
void handle_client_disconnect(int signal);
int retrieve_file_from_server(const char *filename, int server_type);
void get_corresponding_server_path(const char *s1_path, char *server_path, int server_type);
int list_files_in_directory(const char *path, char *file_list, int client_socket);

// Global variables for server connections
ServerInfo s2_info = {"127.0.0.1", S2_PORT};
ServerInfo s3_info = {"127.0.0.1", S3_PORT};
ServerInfo s4_info = {"127.0.0.1", S4_PORT};

int main() {
    int server_socket, client_socket;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_size;
    pid_t child_pid;

    // Create socket
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        perror("Error creating server socket");
        exit(EXIT_FAILURE);
    }

    // Set socket options to reuse address
    int opt = 1;
    if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("Error setting socket options");
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    // Initialize server address
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(S1_PORT);

    // Bind socket to address
    if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Error binding socket");
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    // Listen for connections
    if (listen(server_socket, MAX_PENDING) < 0) {
        perror("Error listening for connections");
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    printf("S1 server started. Listening on port %d...\n", S1_PORT);

    // Set up signal handler for child processes
    signal(SIGCHLD, handle_client_disconnect);

    // Accept and process client connections
    while (1) {
        client_addr_size = sizeof(client_addr);
        client_socket = accept(server_socket, (struct sockaddr *)&client_addr, &client_addr_size);
        
        if (client_socket < 0) {
            if (errno == EINTR) {
                // Interrupted by signal, continue accepting
                continue;
            }
            perror("Error accepting connection");
            continue;
        }

        printf("New client connected: %s:%d\n", 
               inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

        // Fork a child process to handle the client
        child_pid = fork();
        
        if (child_pid < 0) {
            perror("Error forking process");
            close(client_socket);
            continue;
        } else if (child_pid == 0) {
            // Child process
            close(server_socket);  // Close unused server socket in child
            process_client(client_socket);
            close(client_socket);
            exit(EXIT_SUCCESS);
        } else {
            // Parent process
            close(client_socket);  // Close unused client socket in parent
        }
    }

    // Close server socket (will never reach here in this implementation)
    close(server_socket);
    return 0;
}

// Signal handler for child processes
void handle_client_disconnect(int signal) {
    int status;
    while (waitpid(-1, &status, WNOHANG) > 0);
}

// Function to process client requests
void process_client(int client_socket) {
    char command[COMMAND_SIZE];
    int bytes_received;

    // Enter infinite loop to process client commands
    while (1) {
        // Receive command from client
        memset(command, 0, COMMAND_SIZE);
        bytes_received = recv(client_socket, command, COMMAND_SIZE - 1, 0);
        
        if (bytes_received <= 0) {
            if (bytes_received == 0) {
                printf("Client disconnected.\n");
            } else {
                perror("Error receiving command");
            }
            break;
        }

        command[bytes_received] = '\0';
        printf("Received command: %s\n", command);

        // Process command
        if (strncmp(command, "uploadf ", 8) == 0) {
            handle_upload_command(command, client_socket);
        } else if (strncmp(command, "downlf ", 7) == 0) {
            handle_download_command(command, client_socket);
        } else if (strncmp(command, "removef ", 8) == 0) {
            handle_remove_command(command, client_socket);
        } else if (strncmp(command, "downltar ", 9) == 0) {
            handle_download_tar_command(command, client_socket);
        } else if (strncmp(command, "dispfnames ", 11) == 0) {
            handle_display_filenames_command(command, client_socket);
        } else {
            // Invalid command
            char response[] = "ERROR: Invalid command";
            send(client_socket, response, strlen(response), 0);
        }
    }
}

// Function to handle uploadf command
int handle_upload_command(char *command, int client_socket) {
    char filename[MAX_FILENAME];
    char dest_path[MAX_FILEPATH];
    char response[BUFFER_SIZE];
    char *ext;
    
    // Parse command
    if (sscanf(command, "uploadf %s %s", filename, dest_path) != 2) {
        snprintf(response, BUFFER_SIZE, "ERROR: Invalid uploadf command syntax");
        send(client_socket, response, strlen(response), 0);
        return -1;
    }

    // Expand destination path
    char expanded_path[MAX_FILEPATH];
    expand_path(dest_path, expanded_path);
    
    // Verify path is within S1
    if (!is_path_in_s1(expanded_path)) {
        snprintf(response, BUFFER_SIZE, "ERROR: Destination path must be within ~/S1");
        send(client_socket, response, strlen(response), 0);
        return -1;
    }

    // Create directory path if it doesn't exist
    if (create_directory_path(expanded_path) != 0) {
        snprintf(response, BUFFER_SIZE, "ERROR: Failed to create destination directory");
        send(client_socket, response, strlen(response), 0);
        return -1;
    }

    // Determine file type
    ext = get_file_extension(filename);
    if (!ext) {
        snprintf(response, BUFFER_SIZE, "ERROR: File must have an extension");
        send(client_socket, response, strlen(response), 0);
        return -1;
    }

    // Validate file type
    if (strcmp(ext, "c") != 0 && strcmp(ext, "pdf") != 0 && 
        strcmp(ext, "txt") != 0 && strcmp(ext, "zip") != 0) {
        snprintf(response, BUFFER_SIZE, "ERROR: Unsupported file type. Only .c, .pdf, .txt, and .zip are allowed");
        send(client_socket, response, strlen(response), 0);
        return -1;
    }

    // Send acknowledgment to client for file transfer
    snprintf(response, BUFFER_SIZE, "READY_TO_RECEIVE");
    send(client_socket, response, strlen(response), 0);

    // Prepare full path for file
    char filepath[MAX_FILEPATH];
    snprintf(filepath, MAX_FILEPATH, "%s/%s", expanded_path, basename(filename));

    // Receive file from client
    if (receive_file_from_client(filepath, client_socket) != 0) {
        return -1;
    }

    // Transfer file to appropriate server based on extension
    if (strcmp(ext, "c") == 0) {
        // Keep .c files in S1
        snprintf(response, BUFFER_SIZE, "SUCCESS: File uploaded successfully to S1");
    } else {
        int server_type = 0;
        
        if (strcmp(ext, "pdf") == 0) {
            server_type = 2;  // S2
        } else if (strcmp(ext, "txt") == 0) {
            server_type = 3;  // S3
        } else if (strcmp(ext, "zip") == 0) {
            server_type = 4;  // S4
        }
        
        // Transfer file to appropriate server
        transfer_file_to_server(filepath, dest_path, server_type);
        
        // Delete file from S1 after transfer
        if (remove(filepath) != 0) {
            perror("Warning: Failed to delete file from S1 after transfer");
        }
        
        snprintf(response, BUFFER_SIZE, "SUCCESS: File uploaded successfully");
    }

    // Send success response to client
    send(client_socket, response, strlen(response), 0);
    return 0;
}

// Function to handle downlf command
int handle_download_command(char *command, int client_socket) {
    char filepath[MAX_FILEPATH];
    char response[BUFFER_SIZE];
    char *ext;
    
    // Parse command
    if (sscanf(command, "downlf %s", filepath) != 1) {
        snprintf(response, BUFFER_SIZE, "ERROR: Invalid downlf command syntax");
        send(client_socket, response, strlen(response), 0);
        return -1;
    }

    // Expand file path
    char expanded_path[MAX_FILEPATH];
    expand_path(filepath, expanded_path);
    
    // Verify path is within S1
    if (!is_path_in_s1(expanded_path)) {
        snprintf(response, BUFFER_SIZE, "ERROR: File path must be within ~/S1");
        send(client_socket, response, strlen(response), 0);
        return -1;
    }

    // Determine file type
    ext = get_file_extension(expanded_path);
    if (!ext) {
        snprintf(response, BUFFER_SIZE, "ERROR: File must have an extension");
        send(client_socket, response, strlen(response), 0);
        return -1;
    }

    // Validate file type
    if (strcmp(ext, "c") != 0 && strcmp(ext, "pdf") != 0 && 
        strcmp(ext, "txt") != 0 && strcmp(ext, "zip") != 0) {
        snprintf(response, BUFFER_SIZE, "ERROR: Unsupported file type. Only .c, .pdf, .txt, and .zip are allowed");
        send(client_socket, response, strlen(response), 0);
        return -1;
    }

    // Process based on file type
    if (strcmp(ext, "c") == 0) {
        // Check if file exists in S1
        if (access(expanded_path, F_OK) != 0) {
            snprintf(response, BUFFER_SIZE, "ERROR: File not found");
            send(client_socket, response, strlen(response), 0);
            return -1;
        }
        
        // Send acknowledgment to client for file transfer
        snprintf(response, BUFFER_SIZE, "READY_TO_SEND");
        send(client_socket, response, strlen(response), 0);
        
        // Send file to client
        return send_file_to_client(expanded_path, client_socket);
    } else {
        // Determine server type
        int server_type = 0;
        
        if (strcmp(ext, "pdf") == 0) {
            server_type = 2;  // S2
        } else if (strcmp(ext, "txt") == 0) {
            server_type = 3;  // S3
        } else if (strcmp(ext, "zip") == 0) {
            server_type = 4;  // S4
        }
        
        // Retrieve file from appropriate server
        if (retrieve_file_from_server(expanded_path, server_type) != 0) {
            snprintf(response, BUFFER_SIZE, "ERROR: Failed to retrieve file from server");
            send(client_socket, response, strlen(response), 0);
            return -1;
        }
        
        // Send acknowledgment to client for file transfer
        snprintf(response, BUFFER_SIZE, "READY_TO_SEND");
        send(client_socket, response, strlen(response), 0);
        
        // Send file to client
        int result = send_file_to_client(expanded_path, client_socket);
        
        // Delete temporary file after sending
        if (remove(expanded_path) != 0) {
            perror("Warning: Failed to delete temporary file after sending");
        }
        
        return result;
    }
}

// Function to handle removef command
int handle_remove_command(char *command, int client_socket) {
    char filepath[MAX_FILEPATH];
    char response[BUFFER_SIZE];
    char *ext;
    
    // Parse command
    if (sscanf(command, "removef %s", filepath) != 1) {
        snprintf(response, BUFFER_SIZE, "ERROR: Invalid removef command syntax");
        send(client_socket, response, strlen(response), 0);
        return -1;
    }

    // Expand file path
    char expanded_path[MAX_FILEPATH];
    expand_path(filepath, expanded_path);
    
    // Verify path is within S1
    if (!is_path_in_s1(expanded_path)) {
        snprintf(response, BUFFER_SIZE, "ERROR: File path must be within ~/S1");
        send(client_socket, response, strlen(response), 0);
        return -1;
    }

    // Determine file type
    ext = get_file_extension(expanded_path);
    if (!ext) {
        snprintf(response, BUFFER_SIZE, "ERROR: File must have an extension");
        send(client_socket, response, strlen(response), 0);
        return -1;
    }

    // Validate file type
    if (strcmp(ext, "c") != 0 && strcmp(ext, "pdf") != 0 && 
        strcmp(ext, "txt") != 0 && strcmp(ext, "zip") != 0) {
        snprintf(response, BUFFER_SIZE, "ERROR: Unsupported file type. Only .c, .pdf, .txt, and .zip are allowed");
        send(client_socket, response, strlen(response), 0);
        return -1;
    }

    // Process based on file type
    if (strcmp(ext, "c") == 0) {
        // Remove .c file from S1
        if (remove(expanded_path) != 0) {
            snprintf(response, BUFFER_SIZE, "ERROR: Failed to remove file - %s", strerror(errno));
            send(client_socket, response, strlen(response), 0);
            return -1;
        }
    } else {
        // Determine server type and send remove command
        int server_type = 0;
        int server_socket = -1;
        
        if (strcmp(ext, "pdf") == 0) {
            server_type = 2;  // S2
            server_socket = connect_to_server(s2_info.ip, s2_info.port);
        } else if (strcmp(ext, "txt") == 0) {
            server_type = 3;  // S3
            server_socket = connect_to_server(s3_info.ip, s3_info.port);
        } else if (strcmp(ext, "zip") == 0) {
            server_type = 4;  // S4
            server_socket = connect_to_server(s4_info.ip, s4_info.port);
        }
        
        if (server_socket < 0) {
            snprintf(response, BUFFER_SIZE, "ERROR: Failed to connect to server");
            send(client_socket, response, strlen(response), 0);
            return -1;
        }
        
        // Convert S1 path to server path
        char server_path[MAX_FILEPATH];
        get_corresponding_server_path(expanded_path, server_path, server_type);
        
        // Send remove command to server
        char server_command[COMMAND_SIZE];
        snprintf(server_command, COMMAND_SIZE, "REMOVE %s", server_path);
        
        if (send(server_socket, server_command, strlen(server_command), 0) < 0) {
            snprintf(response, BUFFER_SIZE, "ERROR: Failed to send command to server");
            close(server_socket);
            send(client_socket, response, strlen(response), 0);
            return -1;
        }
        
        // Receive response from server
        memset(response, 0, BUFFER_SIZE);
        if (recv(server_socket, response, BUFFER_SIZE - 1, 0) <= 0) {
            snprintf(response, BUFFER_SIZE, "ERROR: Failed to receive response from server");
            close(server_socket);
            send(client_socket, response, strlen(response), 0);
            return -1;
        }
        
        close(server_socket);
        
        if (strncmp(response, "SUCCESS", 7) != 0) {
            send(client_socket, response, strlen(response), 0);
            return -1;
        }
    }

    // Send success response to client
    snprintf(response, BUFFER_SIZE, "SUCCESS: File removed successfully");
    send(client_socket, response, strlen(response), 0);
    return 0;
}

// Function to handle downltar command
int handle_download_tar_command(char *command, int client_socket) {
    char filetype[BUFFER_SIZE];
    char buffer[BUFFER_SIZE] = {0};
    char cmd[BUFFER_SIZE] = {0};
    
    // Parse command
    if (sscanf(command, "downltar %s", filetype) != 1) {
        snprintf(buffer, BUFFER_SIZE, "ERROR: Invalid downltar command syntax");
        send(client_socket, buffer, strlen(buffer), 0);
        return -1;
    }

    printf("[DOWNLTAR] Processing request for filetype: %s\n", filetype);
    
    if (strcmp(filetype, "c") == 0) {
        printf("[C FILES] Handling C files download\n");
        char s1_path[MAX_FILEPATH];
        expand_path(S1_BASE_DIR, s1_path);
        printf("[C FILES] S1 path: %s\n", s1_path);
        
        snprintf(cmd, BUFFER_SIZE, "find \"%s\" -name \"*.c\" -type f | tar -cf - -T - 2>/dev/null", s1_path);
        printf("[C FILES] Command to execute: %s\n", cmd);
        
        FILE *tar_pipe = popen(cmd, "r");
        if (!tar_pipe) {
            printf("[C FILES ERROR] Failed to open tar pipe\n");
            send(client_socket, "TAR_CREATION_FAILED", 19, 0);
            return -1;
        }
        printf("[C FILES] Tar pipe opened successfully\n");
        
        long filesize = 0;
        char temp_buffer[BUFFER_SIZE];
        size_t bytes_read;
        
        printf("[C FILES] Calculating tar size...\n");
        while ((bytes_read = fread(temp_buffer, 1, BUFFER_SIZE, tar_pipe)) > 0) {
            filesize += bytes_read;
        }
        pclose(tar_pipe);
        printf("[C FILES] Calculated tar size: %ld bytes\n", filesize);
        
        if (filesize == 0) {
            printf("[C FILES ERROR] No C files found\n");
            send(client_socket, "NO_FILES", 8, 0);
            return -1;
        }
        
        printf("[C FILES] Reopening tar pipe for transfer\n");
        tar_pipe = popen(cmd, "r");
        if (!tar_pipe) {
            printf("[C FILES ERROR] Failed to reopen tar pipe\n");
            send(client_socket, "TAR_CREATION_FAILED", 19, 0);
            return -1;
        }
        
        snprintf(buffer, BUFFER_SIZE, "%ld", filesize);
        printf("[C FILES] Sending file size: %s\n", buffer);
        if (send(client_socket, buffer, strlen(buffer), 0) <= 0) {
            printf("[C FILES ERROR] Failed to send file size\n");
            pclose(tar_pipe);
            return -1;
        }
        
        memset(buffer, 0, BUFFER_SIZE);
        printf("[C FILES] Waiting for client acknowledgement...\n");
        if (recv(client_socket, buffer, BUFFER_SIZE, 0) <= 0) {
            printf("[C FILES ERROR] Failed to get client ack\n");
            pclose(tar_pipe);
            return -1;
        }
        printf("[C FILES] Received client ack: %s\n", buffer);
        
        size_t total_sent = 0;
        printf("[C FILES] Starting data transfer...\n");
        while ((bytes_read = fread(buffer, 1, BUFFER_SIZE, tar_pipe)) > 0) {
            printf("[C FILES] Read %zu bytes from pipe\n", bytes_read);
            size_t bytes_sent = 0;
            while (bytes_sent < bytes_read) {
                int sent = send(client_socket, buffer + bytes_sent, bytes_read - bytes_sent, 0);
                if (sent <= 0) {
                    printf("[C FILES ERROR] Failed during transfer at %zu/%ld bytes\n", 
                           total_sent + bytes_sent, filesize);
                    pclose(tar_pipe);
                    return -1;
                }
                bytes_sent += sent;
                printf("[C FILES] Sent %d bytes (chunk progress: %zu/%zu)\n", 
                       sent, bytes_sent, bytes_read);
            }
            total_sent += bytes_read;
            printf("[C FILES] Total sent: %zu/%ld bytes (%.1f%%)\n", 
                   total_sent, filesize, (total_sent*100.0)/filesize);
            memset(buffer, 0, BUFFER_SIZE);
        }
        
        printf("[C FILES] Transfer complete. Total bytes sent: %zu\n", total_sent);
        pclose(tar_pipe);
        return 0;
    } 
    else if (strcmp(filetype, "pdf") == 0) {
        printf("[PDF FILES] Handling PDF files download\n");
        printf("[PDF FILES] Connecting to S2 server...\n");
        int server_socket = connect_to_server(s2_info.ip, s2_info.port);
        if (server_socket < 0) {
            printf("[PDF FILES ERROR] Failed to connect to S2 server\n");
            send(client_socket, "SERVER_CONNECTION_FAILED", 24, 0);
            return -1;
        }
        printf("[PDF FILES] Connected to S2 server\n");
        
        snprintf(buffer, BUFFER_SIZE, "CREATETAR %s", filetype);
        printf("[PDF FILES] Sending command to S2: %s\n", buffer);
        if (send(server_socket, buffer, strlen(buffer), 0) <= 0) {
            printf("[PDF FILES ERROR] Failed to send command to S2\n");
            close(server_socket);
            send(client_socket, "SERVER_CONNECTION_FAILED", 24, 0);
            return -1;
        }
        
        memset(buffer, 0, BUFFER_SIZE);
        printf("[PDF FILES] Waiting for response from S2...\n");
        int recv_bytes = recv(server_socket, buffer, BUFFER_SIZE - 1, 0);
        printf("[PDF FILES] Received %d bytes from S2: %s\n", recv_bytes, buffer);

        if (strcmp(buffer, "TAR_CREATION_FAILED") == 0) {
            printf("[PDF FILES ERROR] S2 reported tar creation failed\n");
            close(server_socket);
            send(client_socket, "TAR_CREATION_FAILED", 19, 0);
            return -1;
        }
        
        long filesize = atol(buffer);
        printf("[PDF FILES] S2 reported tar size: %ld bytes\n", filesize);
        if (filesize <= 0) {
            printf("[PDF FILES ERROR] Invalid file size from S2\n");
            close(server_socket);
            send(client_socket, "TAR_CREATION_FAILED", 19, 0);
            return -1;
        }
        
        snprintf(buffer, BUFFER_SIZE, "%ld", filesize);
        printf("[PDF FILES] Sending size to client: %s\n", buffer);
        if (send(client_socket, buffer, strlen(buffer), 0) <= 0) {
            printf("[PDF FILES ERROR] Failed to send size to client\n");
            close(server_socket);
            return -1;
        }
        
        memset(buffer, 0, BUFFER_SIZE);
        printf("[PDF FILES] Waiting for client ready signal...\n");
        if (recv(client_socket, buffer, BUFFER_SIZE, 0) <= 0) {
            printf("[PDF FILES ERROR] Failed to get client ready signal\n");
            close(server_socket);
            return -1;
        }
        printf("[PDF FILES] Received client ready: %s\n", buffer);
        send(server_socket, buffer, strlen(buffer), 0);
        
        size_t bytes_received = 0;
        size_t total_received = 0;
        printf("[PDF FILES] Starting data transfer from S2...\n");
        printf("[PDF FILES] File size is %ld...\n", filesize);
        
        while (total_received < filesize) {
            memset(buffer, 0, BUFFER_SIZE);
            bytes_received = recv(server_socket, buffer, BUFFER_SIZE, 0);
            printf("[PDF FILES] Received %zu bytes from S2\n", bytes_received);
            
            if (bytes_received <= 0) {
                printf("[PDF FILES ERROR] Connection closed by S2 at %zu/%ld bytes\n", 
                       total_received, filesize);
                break;
            }
            
            size_t bytes_sent = 0;
            while (bytes_sent < bytes_received) {
                int sent = send(client_socket, buffer + bytes_sent, bytes_received - bytes_sent, 0);
                if (sent <= 0) {
                    printf("[PDF FILES ERROR] Failed to send to client at %zu/%ld bytes\n", 
                           total_received + bytes_sent, filesize);
                    close(server_socket);
                    return -1;
                }
                bytes_sent += sent;
                printf("[PDF FILES] Sent %d bytes to client\n", sent);
            }
            
            total_received += bytes_received;
            printf("[PDF FILES] Transfer progress: %zu/%ld bytes (%.1f%%)\n", 
                   total_received, filesize, (total_received*100.0)/filesize);
        }
        
        printf("[PDF FILES] Transfer complete. Total bytes: %zu/%ld\n", total_received, filesize);
        close(server_socket);
        return 0;
    }
    else if (strcmp(filetype, "txt") == 0) {
        printf("[TXT FILES] Handling TXT files download\n");
        printf("[TXT FILES] Connecting to S3 server...\n");
        int server_socket = connect_to_server(s3_info.ip, s3_info.port);
        if (server_socket < 0) {
            printf("[TXT FILES ERROR] Failed to connect to S3 server\n");
            send(client_socket, "SERVER_CONNECTION_FAILED", 24, 0);
            return -1;
        }
        printf("[TXT FILES] Connected to S3 server\n");
        
        snprintf(buffer, BUFFER_SIZE, "CREATETAR %s", filetype);
        printf("[TXT FILES] Sending command to S3: %s\n", buffer);
        if (send(server_socket, buffer, strlen(buffer), 0) <= 0) {
            printf("[TXT FILES ERROR] Failed to send command to S3\n");
            close(server_socket);
            send(client_socket, "SERVER_CONNECTION_FAILED", 24, 0);
            return -1;
        }
        
        memset(buffer, 0, BUFFER_SIZE);
        printf("[TXT FILES] Waiting for response from S3...\n");
        int recv_bytes = recv(server_socket, buffer, BUFFER_SIZE - 1, 0);
        printf("[TXT FILES] Received %d bytes from S3: %s\n", recv_bytes, buffer);

        if (strcmp(buffer, "TAR_CREATION_FAILED") == 0) {
            printf("[TXT FILES ERROR] S3 reported tar creation failed\n");
            close(server_socket);
            send(client_socket, "TAR_CREATION_FAILED", 19, 0);
            return -1;
        }
        
        long filesize = atol(buffer);
        printf("[TXT FILES] S3 reported tar size: %ld bytes\n", filesize);
        if (filesize <= 0) {
            printf("[TXT FILES ERROR] Invalid file size from S3\n");
            close(server_socket);
            send(client_socket, "TAR_CREATION_FAILED", 19, 0);
            return -1;
        }
        
        snprintf(buffer, BUFFER_SIZE, "%ld", filesize);
        printf("[TXT FILES] Sending size to client: %s\n", buffer);
        if (send(client_socket, buffer, strlen(buffer), 0) <= 0) {
            printf("[TXT FILES ERROR] Failed to send size to client\n");
            close(server_socket);
            return -1;
        }
        
        memset(buffer, 0, BUFFER_SIZE);
        printf("[TXT FILES] Waiting for client ready signal...\n");
        if (recv(client_socket, buffer, BUFFER_SIZE, 0) <= 0) {
            printf("[TXT FILES ERROR] Failed to get client ready signal\n");
            close(server_socket);
            return -1;
        }
        printf("[TXT FILES] Received client ready: %s\n", buffer);
        send(server_socket, buffer, strlen(buffer), 0);
        
        size_t bytes_received = 0;
        size_t total_received = 0;
        printf("[TXT FILES] Starting data transfer from S3...\n");
        printf("[TXT FILES] File size is %ld...\n", filesize);
        
        while (total_received < filesize) {
            memset(buffer, 0, BUFFER_SIZE);
            bytes_received = recv(server_socket, buffer, BUFFER_SIZE, 0);
            printf("[TXT FILES] Received %zu bytes from S3\n", bytes_received);
            
            if (bytes_received <= 0) {
                printf("[TXT FILES ERROR] Connection closed by S3 at %zu/%ld bytes\n", 
                       total_received, filesize);
                break;
            }
            
            size_t bytes_sent = 0;
            while (bytes_sent < bytes_received) {
                int sent = send(client_socket, buffer + bytes_sent, bytes_received - bytes_sent, 0);
                if (sent <= 0) {
                    printf("[TXT FILES ERROR] Failed to send to client at %zu/%ld bytes\n", 
                           total_received + bytes_sent, filesize);
                    close(server_socket);
                    return -1;
                }
                bytes_sent += sent;
                printf("[TXT FILES] Sent %d bytes to client\n", sent);
            }
            
            total_received += bytes_received;
            printf("[TXT FILES] Transfer progress: %zu/%ld bytes (%.1f%%)\n", 
                   total_received, filesize, (total_received*100.0)/filesize);
        }
        
        printf("[TXT FILES] Transfer complete. Total bytes: %zu/%ld\n", total_received, filesize);
        close(server_socket);
        return 0;
    }
    else if (strcmp(filetype, "zip") == 0) {
        printf("[ZIP FILES] Handling ZIP files download\n");
        printf("[ZIP FILES] Connecting to S4 server...\n");
        int server_socket = connect_to_server(s4_info.ip, s4_info.port);
        if (server_socket < 0) {
            printf("[ZIP FILES ERROR] Failed to connect to S4 server\n");
            send(client_socket, "SERVER_CONNECTION_FAILED", 24, 0);
            return -1;
        }
        printf("[ZIP FILES] Connected to S4 server\n");
        
        snprintf(buffer, BUFFER_SIZE, "CREATETAR %s", filetype);
        printf("[ZIP FILES] Sending command to S4: %s\n", buffer);
        if (send(server_socket, buffer, strlen(buffer), 0) <= 0) {
            printf("[ZIP FILES ERROR] Failed to send command to S4\n");
            close(server_socket);
            send(client_socket, "SERVER_CONNECTION_FAILED", 24, 0);
            return -1;
        }
        
        memset(buffer, 0, BUFFER_SIZE);
        printf("[ZIP FILES] Waiting for response from S4...\n");
        int recv_bytes = recv(server_socket, buffer, BUFFER_SIZE - 1, 0);
        printf("[ZIP FILES] Received %d bytes from S4: %s\n", recv_bytes, buffer);

        if (strcmp(buffer, "TAR_CREATION_FAILED") == 0) {
            printf("[ZIP FILES ERROR] S4 reported tar creation failed\n");
            close(server_socket);
            send(client_socket, "TAR_CREATION_FAILED", 19, 0);
            return -1;
        }
        
        long filesize = atol(buffer);
        printf("[ZIP FILES] S4 reported tar size: %ld bytes\n", filesize);
        if (filesize <= 0) {
            printf("[ZIP FILES ERROR] Invalid file size from S4\n");
            close(server_socket);
            send(client_socket, "TAR_CREATION_FAILED", 19, 0);
            return -1;
        }
        
        snprintf(buffer, BUFFER_SIZE, "%ld", filesize);
        printf("[ZIP FILES] Sending size to client: %s\n", buffer);
        if (send(client_socket, buffer, strlen(buffer), 0) <= 0) {
            printf("[ZIP FILES ERROR] Failed to send size to client\n");
            close(server_socket);
            return -1;
        }
        
        memset(buffer, 0, BUFFER_SIZE);
        printf("[ZIP FILES] Waiting for client ready signal...\n");
        if (recv(client_socket, buffer, BUFFER_SIZE, 0) <= 0) {
            printf("[ZIP FILES ERROR] Failed to get client ready signal\n");
            close(server_socket);
            return -1;
        }
        printf("[ZIP FILES] Received client ready: %s\n", buffer);
        send(server_socket, buffer, strlen(buffer), 0);
        
        size_t bytes_received = 0;
        size_t total_received = 0;
        printf("[ZIP FILES] Starting data transfer from S4...\n");
        printf("[ZIP FILES] File size is %ld...\n", filesize);
        
        while (total_received < filesize) {
            memset(buffer, 0, BUFFER_SIZE);
            bytes_received = recv(server_socket, buffer, BUFFER_SIZE, 0);
            printf("[ZIP FILES] Received %zu bytes from S4\n", bytes_received);
            
            if (bytes_received <= 0) {
                printf("[ZIP FILES ERROR] Connection closed by S4 at %zu/%ld bytes\n", 
                       total_received, filesize);
                break;
            }
            
            size_t bytes_sent = 0;
            while (bytes_sent < bytes_received) {
                int sent = send(client_socket, buffer + bytes_sent, bytes_received - bytes_sent, 0);
                if (sent <= 0) {
                    printf("[ZIP FILES ERROR] Failed to send to client at %zu/%ld bytes\n", 
                           total_received + bytes_sent, filesize);
                    close(server_socket);
                    return -1;
                }
                bytes_sent += sent;
                printf("[ZIP FILES] Sent %d bytes to client\n", sent);
            }
            
            total_received += bytes_received;
            printf("[ZIP FILES] Transfer progress: %zu/%ld bytes (%.1f%%)\n", 
                   total_received, filesize, (total_received*100.0)/filesize);
        }
        
        printf("[ZIP FILES] Transfer complete. Total bytes: %zu/%ld\n", total_received, filesize);
        close(server_socket);
        return 0;
    }
    else {
        printf("[ERROR] Unsupported file type: %s\n", filetype);
        send(client_socket, "ERROR: Unsupported file type", 28, 0);
        return -1;
    }
}

// Function to handle dispfnames command
int handle_display_filenames_command(char *command, int client_socket) {
    char path[MAX_FILEPATH];
    char response[BUFFER_SIZE];
    
    // Parse command
    if (sscanf(command, "dispfnames %s", path) != 1) {
        snprintf(response, BUFFER_SIZE, "ERROR: Invalid dispfnames command syntax");
        send(client_socket, response, strlen(response), 0);
        return -1;
    }

    // Expand path
    char expanded_path[MAX_FILEPATH];
    expand_path(path, expanded_path);
    
    // Verify path is within S1
    if (!is_path_in_s1(expanded_path)) {
        snprintf(response, BUFFER_SIZE, "ERROR: Path must be within ~/S1");
        send(client_socket, response, strlen(response), 0);
        return -1;
    }

    // Check if directory exists
    struct stat st;
    if (stat(expanded_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        snprintf(response, BUFFER_SIZE, "ERROR: Directory not found or is not a directory");
        send(client_socket, response, strlen(response), 0);
        return -1;
    }

    // Buffer to store file list
    char file_list[BUFFER_SIZE * 4];
    memset(file_list, 0, BUFFER_SIZE * 4);

    // List files in directory
    if (list_files_in_directory(expanded_path, file_list, client_socket) != 0) {
        snprintf(response, BUFFER_SIZE, "ERROR: Failed to list files");
        send(client_socket, response, strlen(response), 0);
        return -1;
    }

    return 0;
}

// Function to list files in a directory
int list_files_in_directory(const char *path, char *file_list, int client_socket) {
    // Get .c files from S1
    DIR *dir = opendir(path);
    if (!dir) {
        return -1;
    }

    // Temporary files to store sorted file lists
    char c_files[BUFFER_SIZE] = "";
    char pdf_files[BUFFER_SIZE] = "";
    char txt_files[BUFFER_SIZE] = "";
    char zip_files[BUFFER_SIZE] = "";
    
    // Get .c files from S1
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG) {
            char *ext = get_file_extension(entry->d_name);
            if (ext && strcmp(ext, "c") == 0) {
                strcat(c_files, entry->d_name);
                strcat(c_files, "\n");
            }
        }
    }
    closedir(dir);

    // Connect to servers to get file lists
    int server_socket;
    char server_command[COMMAND_SIZE];
    char response[BUFFER_SIZE];
    char server_path[MAX_FILEPATH];
    
    // Get .pdf files from S2
    server_socket = connect_to_server(s2_info.ip, s2_info.port);
    if (server_socket >= 0) {
        get_corresponding_server_path(path, server_path, 2);
        snprintf(server_command, COMMAND_SIZE, "LIST %s pdf", server_path);
        
        if (send(server_socket, server_command, strlen(server_command), 0) >= 0) {
            memset(response, 0, BUFFER_SIZE);
            if (recv(server_socket, response, BUFFER_SIZE - 1, 0) > 0) {
                strcpy(pdf_files, response);
            }
        }
        close(server_socket);
    }
    
    // Get .txt files from S3
    server_socket = connect_to_server(s3_info.ip, s3_info.port);
    if (server_socket >= 0) {
        get_corresponding_server_path(path, server_path, 3);
        snprintf(server_command, COMMAND_SIZE, "LIST %s txt", server_path);
        
        if (send(server_socket, server_command, strlen(server_command), 0) >= 0) {
            memset(response, 0, BUFFER_SIZE);
            if (recv(server_socket, response, BUFFER_SIZE - 1, 0) > 0) {
                strcpy(txt_files, response);
            }
        }
        close(server_socket);
    }
    
    // Get .zip files from S4
    server_socket = connect_to_server(s4_info.ip, s4_info.port);
    if (server_socket >= 0) {
        get_corresponding_server_path(path, server_path, 4);
        snprintf(server_command, COMMAND_SIZE, "LIST %s zip", server_path);
        
        if (send(server_socket, server_command, strlen(server_command), 0) >= 0) {
            memset(response, 0, BUFFER_SIZE);
            if (recv(server_socket, response, BUFFER_SIZE - 1, 0) > 0) {
                strcpy(zip_files, response);
            }
        }
        close(server_socket);
    }

    // Combine file lists
    strcpy(file_list, c_files);
    strcat(file_list, pdf_files);
    strcat(file_list, txt_files);
    strcat(file_list, zip_files);

    // Send combined list to client
    char ready_msg[] = "FILES_COMING";
    send(client_socket, ready_msg, strlen(ready_msg), 0);
    
    // Wait for client acknowledgment
    memset(response, 0, BUFFER_SIZE);
    if (recv(client_socket, response, BUFFER_SIZE - 1, 0) <= 0 || 
        strcmp(response, "READY") != 0) {
        return -1;
    }
    
    // Send file list
    if (strlen(file_list) == 0) {
        char no_files[] = "No files found in this directory";
        send(client_socket, no_files, strlen(no_files), 0);
    } else {
        send(client_socket, file_list, strlen(file_list), 0);
    }
    
    return 0;
}

// Function to transfer file to another server
void transfer_file_to_server(const char *filename, const char *dest_path, int server_type) {
    int server_socket = -1;
    
    // Connect to appropriate server
    switch (server_type) {
        case 2:  // S2
            server_socket = connect_to_server(s2_info.ip, s2_info.port);
            break;
        case 3:  // S3
            server_socket = connect_to_server(s3_info.ip, s3_info.port);
            break;
        case 4:  // S4
            server_socket = connect_to_server(s4_info.ip, s4_info.port);
            break;
        default:
            return;
    }
    
    if (server_socket < 0) {
        perror("Error connecting to server");
        return;
    }
    
    // Prepare server destination path
    char server_dest_path[MAX_FILEPATH];
    char *modified_path = strdup(dest_path);
    
    // Replace S1 with appropriate server directory
    char *s1_pos = strstr(modified_path, "S1");
    if (s1_pos) {
        char server_name[3];
        snprintf(server_name, 3, "S%d", server_type);
        memcpy(s1_pos, server_name, 2);
    }
    
    strcpy(server_dest_path, modified_path);
    free(modified_path);
    
    // Send upload command to server
    char server_command[COMMAND_SIZE];
    snprintf(server_command, COMMAND_SIZE, "RECEIVE %s %s", basename((char *)filename), server_dest_path);
    
    if (send(server_socket, server_command, strlen(server_command), 0) < 0) {
        perror("Error sending command to server");
        close(server_socket);
        return;
    }
    
    // Wait for server response
    char response[BUFFER_SIZE];
    memset(response, 0, BUFFER_SIZE);
    
    if (recv(server_socket, response, BUFFER_SIZE - 1, 0) <= 0 || 
        strcmp(response, "READY_TO_RECEIVE") != 0) {
        perror("Error receiving response from server");
        close(server_socket);
        return;
    }
    
    // Send file to server
    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        perror("Error opening file");
        close(server_socket);
        return;
    }
    
    char buffer[BUFFER_SIZE];
    size_t bytes_read;
    
    while ((bytes_read = fread(buffer, 1, BUFFER_SIZE, fp)) > 0) {
        if (send(server_socket, buffer, bytes_read, 0) < 0) {
            perror("Error sending file to server");
            fclose(fp);
            close(server_socket);
            return;
        }
    }
    
    fclose(fp);
    close(server_socket);
}

// Function to retrieve file from another server
int retrieve_file_from_server(const char *filename, int server_type) {
    int server_socket = -1;
    
    // Connect to appropriate server
    switch (server_type) {
        case 2:  // S2
            server_socket = connect_to_server(s2_info.ip, s2_info.port);
            break;
        case 3:  // S3
            server_socket = connect_to_server(s3_info.ip, s3_info.port);
            break;
        case 4:  // S4
            server_socket = connect_to_server(s4_info.ip, s4_info.port);
            break;
        default:
            return -1;
    }
    
    if (server_socket < 0) {
        perror("Error connecting to server");
        return -1;
    }
    
    // Prepare server file path
    char server_filepath[MAX_FILEPATH];
    get_corresponding_server_path(filename, server_filepath, server_type);
    
    // Send download command to server
    char server_command[COMMAND_SIZE];
    snprintf(server_command, COMMAND_SIZE, "SEND %s", server_filepath);
    
    if (send(server_socket, server_command, strlen(server_command), 0) < 0) {
        perror("Error sending command to server");
        close(server_socket);
        return -1;
    }
    
    // Wait for server response
    char response[BUFFER_SIZE];
    memset(response, 0, BUFFER_SIZE);
    
    if (recv(server_socket, response, BUFFER_SIZE - 1, 0) <= 0 || 
        strncmp(response, "READY_TO_SEND", 13) != 0) {
        perror("Error receiving response from server");
        close(server_socket);
        return -1;
    }
    
    // Create directory path if needed
    char *dir_path = strdup(filename);
    char *last_slash = strrchr(dir_path, '/');
    if (last_slash) {
        *last_slash = '\0';
        create_directory_path(dir_path);
    }
    free(dir_path);
    
    // Receive file from server
    FILE *fp = fopen(filename, "wb");
    if (!fp) {
        perror("Error creating file");
        close(server_socket);
        return -1;
    }
    
    char buffer[BUFFER_SIZE];
    ssize_t bytes_read;
    
    while ((bytes_read = recv(server_socket, buffer, BUFFER_SIZE, 0)) > 0) {
        if (fwrite(buffer, 1, bytes_read, fp) != bytes_read) {
            perror("Error writing to file");
            fclose(fp);
            close(server_socket);
            remove(filename);
            return -1;
        }
        
        // Check if end of file
        if (bytes_read < BUFFER_SIZE) {
            break;
        }
    }
    
    fclose(fp);
    close(server_socket);
    
    if (bytes_read < 0) {
        perror("Error receiving file from server");
        remove(filename);
        return -1;
    }
    
    return 0;
}

// Function to send file to client
int send_file_to_client(const char *filepath, int client_socket) {
    FILE *fp = fopen(filepath, "rb");
    if (!fp) {
        char response[BUFFER_SIZE];
        snprintf(response, BUFFER_SIZE, "ERROR: File not found or cannot be opened");
        send(client_socket, response, strlen(response), 0);
        return -1;
    }
    
    char buffer[BUFFER_SIZE];
    size_t bytes_read;
    
    while ((bytes_read = fread(buffer, 1, BUFFER_SIZE, fp)) > 0) {
        if (send(client_socket, buffer, bytes_read, 0) < 0) {
            perror("Error sending file to client");
            fclose(fp);
            return -1;
        }
    }
    
    fclose(fp);
    return 0;
}

// Function to receive file from client
int receive_file_from_client(const char *filepath, int client_socket) {
    // Create directory path if needed
    char *dir_path = strdup(filepath);
    char *last_slash = strrchr(dir_path, '/');
    if (last_slash) {
        *last_slash = '\0';
        create_directory_path(dir_path);
    }
    free(dir_path);
    
    FILE *fp = fopen(filepath, "wb");
    if (!fp) {
        char response[BUFFER_SIZE];
        snprintf(response, BUFFER_SIZE, "ERROR: Failed to create file");
        send(client_socket, response, strlen(response), 0);
        return -1;
    }
    
    char buffer[BUFFER_SIZE];
    ssize_t bytes_read;
    
    while ((bytes_read = recv(client_socket, buffer, BUFFER_SIZE, 0)) > 0) {
        if (fwrite(buffer, 1, bytes_read, fp) != bytes_read) {
            perror("Error writing to file");
            fclose(fp);
            remove(filepath);
            return -1;
        }
        
        // Check if end of file
        if (bytes_read < BUFFER_SIZE) {
            break;
        }
    }
    
    fclose(fp);
    
    if (bytes_read < 0) {
        perror("Error receiving file from client");
        remove(filepath);
        return -1;
    }
    
    return 0;
}

// Function to connect to another server
int connect_to_server(const char *server_ip, int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("Error creating socket");
        return -1;
    }
    
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
        perror("Invalid address or address not supported");
        close(sock);
        return -1;
    }
    
    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Connection failed");
        close(sock);
        return -1;
    }
    
    return sock;
}

// Function to create directory path
int create_directory_path(const char *path) {
    char tmp[MAX_FILEPATH];
    char *p = NULL;
    size_t len;
    
    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    
    // Remove trailing slash if present
    if (tmp[len - 1] == '/') {
        tmp[len - 1] = 0;
    }
    
    // Create directory path recursively
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                return -1;
            }
            *p = '/';
        }
    }
    
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    
    return 0;
}

// Function to expand path (replace ~ with home directory)
void expand_path(const char *path, char *expanded_path) {
    if (path[0] == '~') {
        char *home = getenv("HOME");
        if (home) {
            snprintf(expanded_path, MAX_FILEPATH, "%s%s", home, path + 1);
        } else {
            strcpy(expanded_path, path);
        }
    } else {
        strcpy(expanded_path, path);
    }
}

// Function to check if path is within S1
int is_path_in_s1(const char *path) {
    char s1_dir[MAX_FILEPATH];
    expand_path(S1_BASE_DIR, s1_dir);
    
    // Check if path starts with S1 directory
    return strncmp(path, s1_dir, strlen(s1_dir)) == 0;
}

// Function to get file extension
char* get_file_extension(const char *filename) {
    char *dot = strrchr(filename, '.');
    if (!dot || dot == filename) {
        return NULL;
    }
    return dot + 1;
}

// Function to get corresponding server path
void get_corresponding_server_path(const char *s1_path, char *server_path, int server_type) {
    char s1_base[MAX_FILEPATH];
    expand_path(S1_BASE_DIR, s1_base);
    
    // Replace S1 with appropriate server directory
    char server_base[MAX_FILEPATH];
    snprintf(server_base, MAX_FILEPATH, "~/S%d", server_type);
    
    char expanded_server_base[MAX_FILEPATH];
    expand_path(server_base, expanded_server_base);
    
    // Get relative path from S1 base
    const char *relative_path = s1_path + strlen(s1_base);
    
    // Combine server base with relative path
    snprintf(server_path, MAX_FILEPATH, "%s%s", expanded_server_base, relative_path);
}
