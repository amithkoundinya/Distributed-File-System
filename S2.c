#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <libgen.h>

#define S2_PORT 8387
#define BUFFER_SIZE 4096
#define CMD_SIZE 1024
#define PATH_MAX_LEN 1024
#define FILENAME_MAX_LEN 256
#define MAX_CONNECTIONS 10
#define S2_BASE_DIR "~/S2"

// Function declarations
void process_s1_request(int s1_socket);
int receive_file(int socket, const char *filepath);
int send_file(int socket, const char *filepath);
int create_directory_recursive(const char *path);
void expand_tilde_path(const char *path, char *expanded);
int is_valid_path(const char *path);
void get_sorted_files(const char *dirpath, const char *extension, char *result);
int create_tar_file(const char *extension, char *tarfile);
char* get_file_extension(const char *filename);
int compare_strings(const void *a, const void *b);

int main() {
    int server_socket, client_socket;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);
    
    // Create socket
    if ((server_socket = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("S2: Socket creation failed");
        exit(EXIT_FAILURE);
    }
    
    // Set socket options for address reuse
    int opt = 1;
    if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("S2: setsockopt failed");
        close(server_socket);
        exit(EXIT_FAILURE);
    }
    
    // Initialize server address
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(S2_PORT);
    
    // Bind socket to address
    if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("S2: Bind failed");
        close(server_socket);
        exit(EXIT_FAILURE);
    }
    
    // Listen for incoming connections
    if (listen(server_socket, MAX_CONNECTIONS) < 0) {
        perror("S2: Listen failed");
        close(server_socket);
        exit(EXIT_FAILURE);
    }
    
    printf("S2 server started. Listening on port %d...\n", S2_PORT);
    
    // Create S2 base directory if it doesn't exist
    char expanded_base[PATH_MAX_LEN];
    expand_tilde_path(S2_BASE_DIR, expanded_base);
    create_directory_recursive(expanded_base);
    
    // Accept and handle client connections
    while (1) {
        client_socket = accept(server_socket, (struct sockaddr *)&client_addr, &client_len);
        if (client_socket < 0) {
            perror("S2: Accept failed");
            continue;
        }
        
        printf("S2: Connection accepted from %s:%d\n", 
               inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
        
        // Process the client request
        process_s1_request(client_socket);
        
        // Close the connection
        close(client_socket);
    }
    
    // Close the server socket (never reached in this implementation)
    close(server_socket);
    return 0;
}

// Function to process requests from S1
void process_s1_request(int s1_socket) {
    char command[CMD_SIZE];
    int bytes_received = recv(s1_socket, command, CMD_SIZE - 1, 0);
    
    if (bytes_received <= 0) {
        perror("S2: Error receiving command");
        return;
    }
    
    command[bytes_received] = '\0';
    printf("S2: Received command: %s\n", command);
    
    // Parse command
    char cmd_type[20] = {0};
    char arg1[PATH_MAX_LEN] = {0};
    char arg2[PATH_MAX_LEN] = {0};
    
    sscanf(command, "%s %s %s", cmd_type, arg1, arg2);
    
    // Handle different command types
    if (strcmp(cmd_type, "RECEIVE") == 0) {
        // Command format: RECEIVE <filename> <destination_path>
        char filepath[PATH_MAX_LEN];
        char expanded_path[PATH_MAX_LEN];
        
        expand_tilde_path(arg2, expanded_path);
        snprintf(filepath, PATH_MAX_LEN, "%s/%s", expanded_path, arg1);
        
        // Create directory structure
        char *dir_path = strdup(filepath);
        char *last_slash = strrchr(dir_path, '/');
        if (last_slash) {
            *last_slash = '\0';
            create_directory_recursive(dir_path);
        }
        free(dir_path);
        
        // Acknowledge ready to receive
        send(s1_socket, "READY_TO_RECEIVE", 16, 0);
        
        // Receive the file
        if (receive_file(s1_socket, filepath) == 0) {
            printf("S2: File successfully received and saved to %s\n", filepath);
        } else {
            printf("S2: Failed to receive file\n");
        }
    }
    else if (strcmp(cmd_type, "SEND") == 0) {
        // Command format: SEND <filepath>
        char expanded_path[PATH_MAX_LEN];
        expand_tilde_path(arg1, expanded_path);
        
        // Check if file exists
        if (access(expanded_path, F_OK) != 0) {
            send(s1_socket, "ERROR: File not found", 21, 0);
            return;
        }
        
        // Acknowledge ready to send
        send(s1_socket, "READY_TO_SEND", 13, 0);
        
        // Send the file
        if (send_file(s1_socket, expanded_path) == 0) {
            printf("S2: File successfully sent: %s\n", expanded_path);
        } else {
            printf("S2: Failed to send file\n");
        }
    }
    else if (strcmp(cmd_type, "REMOVE") == 0) {
        // Command format: REMOVE <filepath>
        char expanded_path[PATH_MAX_LEN];
        expand_tilde_path(arg1, expanded_path);
        
        // Check if file exists
        if (access(expanded_path, F_OK) != 0) {
            send(s1_socket, "ERROR: File not found", 21, 0);
            return;
        }
        
        // Remove the file
        if (remove(expanded_path) == 0) {
            send(s1_socket, "SUCCESS: File removed", 21, 0);
            printf("S2: File successfully removed: %s\n", expanded_path);
        } else {
            char error_msg[BUFFER_SIZE];
            snprintf(error_msg, BUFFER_SIZE, "ERROR: Failed to remove file - %s", strerror(errno));
            send(s1_socket, error_msg, strlen(error_msg), 0);
            printf("S2: Failed to remove file: %s\n", expanded_path);
        }
    }
    else if (strcmp(cmd_type, "CREATETAR") == 0) {
        // Command format: CREATETAR <filetype>
        if (strcmp(arg1, "pdf") != 0) {
            printf("S2: Invalid filetype requested: %s\n", arg1);
            send(s1_socket, "INVALID_FILETYPE", 16, 0);
            return;
        }

        char s2_path[PATH_MAX_LEN];
        expand_tilde_path(S2_BASE_DIR, s2_path);
        printf("S2: Looking for PDF files in: %s\n", s2_path);

        // Create the tar command
        char command[CMD_SIZE];
        snprintf(command, CMD_SIZE, 
                 "find \"%s\" -name \"*.pdf\" -type f | tar -cf - -T - 2>/dev/null", 
                 s2_path);
        printf("S2: Executing command: %s\n", command);

        // First pass to calculate size
        FILE *tar_pipe = popen(command, "r");
        if (!tar_pipe) {
            printf("S2: Failed to open pipe for tar creation\n");
            send(s1_socket, "TAR_CREATION_FAILED", 19, 0);
            return;
        }

        long filesize = 0;
        char temp_buffer[BUFFER_SIZE];
        size_t bytes_read;

        while ((bytes_read = fread(temp_buffer, 1, BUFFER_SIZE, tar_pipe)) > 0) {
            filesize += bytes_read;
        }
        pclose(tar_pipe);

        if (filesize == 0) {
            printf("S2: No PDF files found to tar\n");
            send(s1_socket, "NO_FILES", 8, 0);
            return;
        }

        // Reopen pipe for actual transfer
        tar_pipe = popen(command, "r");
        if (!tar_pipe) {
            printf("S2: Failed to reopen tar pipe\n");
            send(s1_socket, "TAR_CREATION_FAILED", 19, 0);
            return;
        }

        // Send size to client
        char size_buffer[BUFFER_SIZE];
        snprintf(size_buffer, BUFFER_SIZE, "%ld", filesize);
        printf("S2: Sending file size: %s bytes\n", size_buffer);
        if (send(s1_socket, size_buffer, strlen(size_buffer), 0) <= 0) {
            printf("S2: Failed to send file size\n");
            pclose(tar_pipe);
            return;
        }

        // Wait for client ready signal
        char ready_buffer[BUFFER_SIZE] = {0};
        if (recv(s1_socket, ready_buffer, BUFFER_SIZE - 1, 0) <= 0) {
            printf("S2: Failed to get client ready signal\n");
            pclose(tar_pipe);
            return;
        }

        // Stream tar data to client
        size_t total_sent = 0;
        while ((bytes_read = fread(temp_buffer, 1, BUFFER_SIZE, tar_pipe)) > 0) {
            size_t bytes_sent = 0;
            while (bytes_sent < bytes_read) {
                int sent = send(s1_socket, temp_buffer + bytes_sent, bytes_read - bytes_sent, 0);
                if (sent <= 0) {
                    printf("S2: Failed during data transfer at %zu/%ld bytes\n", 
                           total_sent + bytes_sent, filesize);
                    pclose(tar_pipe);
                    return;
                }
                bytes_sent += sent;
            }
            total_sent += bytes_read;
        }

        printf("S2: Sent %zu/%ld bytes of tar data\n", total_sent, filesize);
        pclose(tar_pipe);
    }
    else if (strcmp(cmd_type, "LIST") == 0) {
        // Command format: LIST <dirpath> <extension>
        char expanded_path[PATH_MAX_LEN];
        expand_tilde_path(arg1, expanded_path);
        
        // Check if directory exists
        struct stat st;
        if (stat(expanded_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
            send(s1_socket, "", 0, 0);  // Send empty response
            return;
        }
        
        // Get sorted list of files
        char result[BUFFER_SIZE] = {0};
        get_sorted_files(expanded_path, arg2, result);
        
        // Send the result
        send(s1_socket, result, strlen(result), 0);
        printf("S2: File list sent for directory: %s\n", expanded_path);
    }
    else {
        // Unknown command
        send(s1_socket, "ERROR: Unknown command", 22, 0);
        printf("S2: Unknown command received: %s\n", cmd_type);
    }
}

// Function to receive a file from socket
int receive_file(int socket, const char *filepath) {
    FILE *fp = fopen(filepath, "wb");
    if (!fp) {
        perror("S2: Error opening file for writing");
        return -1;
    }
    
    char buffer[BUFFER_SIZE];
    ssize_t bytes_received;
    
    while ((bytes_received = recv(socket, buffer, BUFFER_SIZE, 0)) > 0) {
        if (fwrite(buffer, 1, bytes_received, fp) != bytes_received) {
            perror("S2: Error writing to file");
            fclose(fp);
            return -1;
        }
        
        // Check if this is the end of the file
        if (bytes_received < BUFFER_SIZE) {
            break;
        }
    }
    
    fclose(fp);
    
    if (bytes_received < 0) {
        perror("S2: Error receiving file data");
        return -1;
    }
    
    return 0;
}

// Function to send a file over socket
int send_file(int socket, const char *filepath) {
    FILE *fp = fopen(filepath, "rb");
    if (!fp) {
        perror("S2: Error opening file for reading");
        return -1;
    }
    
    char buffer[BUFFER_SIZE];
    size_t bytes_read;
    
    while ((bytes_read = fread(buffer, 1, BUFFER_SIZE, fp)) > 0) {
        if (send(socket, buffer, bytes_read, 0) < 0) {
            perror("S2: Error sending file data");
            fclose(fp);
            return -1;
        }
    }
    
    fclose(fp);
    return 0;
}

// Function to create directory hierarchy recursively
int create_directory_recursive(const char *path) {
    char tmp[PATH_MAX_LEN];
    char *p = NULL;
    size_t len;
    
    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    
    // Remove trailing slash if present
    if (tmp[len - 1] == '/') {
        tmp[len - 1] = 0;
    }
    
    // Create each directory in the path
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                return -1;
            }
            *p = '/';
        }
    }
    
    // Create the final directory
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    
    return 0;
}

// Function to expand tilde in path
void expand_tilde_path(const char *path, char *expanded) {
    if (path[0] == '~') {
        const char *home = getenv("HOME");
        if (home) {
            snprintf(expanded, PATH_MAX_LEN, "%s%s", home, path + 1);
        } else {
            strcpy(expanded, path);
        }
    } else {
        strcpy(expanded, path);
    }
}

// Function to check if path is within S2 base directory
int is_valid_path(const char *path) {
    char base_dir[PATH_MAX_LEN];
    expand_tilde_path(S2_BASE_DIR, base_dir);
    return strncmp(path, base_dir, strlen(base_dir)) == 0;
}

// Function to get sorted list of files with specific extension
void get_sorted_files(const char *dirpath, const char *extension, char *result) {
    DIR *dir = opendir(dirpath);
    if (!dir) {
        return;
    }
    
    // Dynamic array to store filenames
    char **filenames = NULL;
    int count = 0;
    int capacity = 10;
    
    filenames = (char **)malloc(capacity * sizeof(char *));
    if (!filenames) {
        closedir(dir);
        return;
    }
    
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG) {
            char *ext = get_file_extension(entry->d_name);
            if (ext && strcmp(ext, extension) == 0) {
                // Resize array if needed
                if (count == capacity) {
                    capacity *= 2;
                    char **new_filenames = (char **)realloc(filenames, capacity * sizeof(char *));
                    if (!new_filenames) {
                        break;
                    }
                    filenames = new_filenames;
                }
                
                // Store filename
                filenames[count] = strdup(entry->d_name);
                if (filenames[count]) {
                    count++;
                }
            }
        }
    }
    
    closedir(dir);
    
    // Sort filenames alphabetically
    qsort(filenames, count, sizeof(char *), compare_strings);
    
    // Combine into result string
    result[0] = '\0';
    for (int i = 0; i < count; i++) {
        strcat(result, filenames[i]);
        strcat(result, "\n");
        free(filenames[i]);
    }
    
    free(filenames);
}

// Function to create tar file of all files with specific extension
int create_tar_file(const char *extension, char *tarfile) {
    char expanded_base[PATH_MAX_LEN];
    expand_tilde_path(S2_BASE_DIR, expanded_base);
    
    // Create tar command
    char tar_command[CMD_SIZE];
    snprintf(tar_command, CMD_SIZE, 
             "find %s -name \"*.%s\" -type f -print0 | tar -cvf %s --null -T -", 
             expanded_base, extension, tarfile);
    
    return system(tar_command);
}

// Function to get file extension
char* get_file_extension(const char *filename) {
    char *dot = strrchr(filename, '.');
    if (!dot || dot == filename) {
        return NULL;
    }
    return dot + 1;
}

// Compare function for qsort
int compare_strings(const void *a, const void *b) {
    return strcmp(*(const char **)a, *(const char **)b);
}
