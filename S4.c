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
#define MAX_PENDING 10
#define S4_PORT 8389
#define S4_BASE_DIR "~/S4"

// Function prototypes
void handle_client_disconnect(int signal);
void process_client_request(int client_socket);
int create_directory_path(const char *path);
int handle_receive_command(char *command, int client_socket);
int handle_send_command(char *command, int client_socket);
int handle_remove_command(char *command, int client_socket);
int handle_list_command(char *command, int client_socket);
int handle_create_tar_command(char *command, int client_socket);
int send_file(const char *filepath, int client_socket);
int receive_file(const char *filepath, int client_socket);
void expand_path(const char *path, char *expanded_path);
char* get_file_extension(const char *filename);
int list_files_in_directory(const char *path, const char *extension, char *file_list);

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

    // Allow port reuse
    int opt = 1;
    if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("Error setting socket options");
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    // Initialize server address structure
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(S4_PORT);

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

    printf("S4 server started. Listening on port %d...\n", S4_PORT);

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

        printf("New connection from %s:%d\n", 
               inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

        // Fork a child process to handle the client request
        child_pid = fork();
        
        if (child_pid < 0) {
            perror("Error forking process");
            close(client_socket);
            continue;
        } else if (child_pid == 0) {
            // Child process
            close(server_socket);  // Close unused server socket in child
            process_client_request(client_socket);
            close(client_socket);
            exit(EXIT_SUCCESS);
        } else {
            // Parent process
            close(client_socket);  // Close unused client socket in parent
        }
    }

    // Close server socket (this will never be reached with the current implementation)
    close(server_socket);
    return 0;
}

// Signal handler for terminated child processes
void handle_client_disconnect(int signal) {
    int status;
    while (waitpid(-1, &status, WNOHANG) > 0);
}

// Process client requests
void process_client_request(int client_socket) {
    char command[COMMAND_SIZE];
    int bytes_received;

    // Receive command from client (S1 server)
    memset(command, 0, COMMAND_SIZE);
    bytes_received = recv(client_socket, command, COMMAND_SIZE - 1, 0);
    
    if (bytes_received <= 0) {
        if (bytes_received == 0) {
            printf("Client disconnected\n");
        } else {
            perror("Error receiving command");
        }
        return;
    }

    command[bytes_received] = '\0';
    printf("Received command: %s\n", command);

    // Process different command types
    if (strncmp(command, "RECEIVE ", 8) == 0) {
        handle_receive_command(command, client_socket);
    } else if (strncmp(command, "SEND ", 5) == 0) {
        handle_send_command(command, client_socket);
    } else if (strncmp(command, "REMOVE ", 7) == 0) {
        handle_remove_command(command, client_socket);
    } else if (strncmp(command, "LIST ", 5) == 0) {
        handle_list_command(command, client_socket);
    } else if (strncmp(command, "CREATE_TAR ", 11) == 0) {
        handle_create_tar_command(command, client_socket);
    } else {
        // Invalid command
        char response[] = "ERROR: Invalid command";
        send(client_socket, response, strlen(response), 0);
    }
}

// Handle RECEIVE command (upload file from S1 to S4)
int handle_receive_command(char *command, int client_socket) {
    char filename[MAX_FILENAME];
    char dest_path[MAX_FILEPATH];
    char response[BUFFER_SIZE];
    
    // Parse command
    if (sscanf(command, "RECEIVE %s %s", filename, dest_path) != 2) {
        snprintf(response, BUFFER_SIZE, "ERROR: Invalid RECEIVE command syntax");
        send(client_socket, response, strlen(response), 0);
        return -1;
    }

    // Expand destination path
    char expanded_path[MAX_FILEPATH];
    expand_path(dest_path, expanded_path);
    
    // Create directory path if it doesn't exist
    if (create_directory_path(expanded_path) != 0) {
        snprintf(response, BUFFER_SIZE, "ERROR: Failed to create destination directory");
        send(client_socket, response, strlen(response), 0);
        return -1;
    }

    // Prepare full path for file
    char filepath[MAX_FILEPATH];
    snprintf(filepath, MAX_FILEPATH, "%s/%s", expanded_path, filename);

    // Send ready signal to S1
    snprintf(response, BUFFER_SIZE, "READY_TO_RECEIVE");
    send(client_socket, response, strlen(response), 0);

    // Receive file from S1
    if (receive_file(filepath, client_socket) != 0) {
        snprintf(response, BUFFER_SIZE, "ERROR: Failed to receive file");
        send(client_socket, response, strlen(response), 0);
        return -1;
    }

    // Send success response
    snprintf(response, BUFFER_SIZE, "SUCCESS: File received and stored successfully");
    send(client_socket, response, strlen(response), 0);
    
    return 0;
}

// Handle SEND command (send file from S4 to S1)
int handle_send_command(char *command, int client_socket) {
    char filepath[MAX_FILEPATH];
    char response[BUFFER_SIZE];
    
    // Parse command
    if (sscanf(command, "SEND %s", filepath) != 1) {
        snprintf(response, BUFFER_SIZE, "ERROR: Invalid SEND command syntax");
        send(client_socket, response, strlen(response), 0);
        return -1;
    }

    // Expand file path
    char expanded_path[MAX_FILEPATH];
    expand_path(filepath, expanded_path);

    // Check if file exists
    if (access(expanded_path, F_OK) != 0) {
        snprintf(response, BUFFER_SIZE, "ERROR: File not found");
        send(client_socket, response, strlen(response), 0);
        return -1;
    }

    // Send ready signal to S1
    snprintf(response, BUFFER_SIZE, "READY_TO_SEND");
    send(client_socket, response, strlen(response), 0);

    // Send file to S1
    return send_file(expanded_path, client_socket);
}

// Handle REMOVE command (delete file in S4)
int handle_remove_command(char *command, int client_socket) {
    char filepath[MAX_FILEPATH];
    char response[BUFFER_SIZE];
    
    // Parse command
    if (sscanf(command, "REMOVE %s", filepath) != 1) {
        snprintf(response, BUFFER_SIZE, "ERROR: Invalid REMOVE command syntax");
        send(client_socket, response, strlen(response), 0);
        return -1;
    }

    // Expand file path
    char expanded_path[MAX_FILEPATH];
    expand_path(filepath, expanded_path);

    // Remove file
    if (remove(expanded_path) != 0) {
        snprintf(response, BUFFER_SIZE, "ERROR: Failed to remove file - %s", strerror(errno));
        send(client_socket, response, strlen(response), 0);
        return -1;
    }

    // Send success response
    snprintf(response, BUFFER_SIZE, "SUCCESS: File removed successfully");
    send(client_socket, response, strlen(response), 0);
    
    return 0;
}

// Handle LIST command (list files in directory)
int handle_list_command(char *command, int client_socket) {
    char path[MAX_FILEPATH];
    char extension[10];
    char response[BUFFER_SIZE];
    
    // Parse command
    if (sscanf(command, "LIST %s %s", path, extension) != 2) {
        snprintf(response, BUFFER_SIZE, "ERROR: Invalid LIST command syntax");
        send(client_socket, response, strlen(response), 0);
        return -1;
    }

    // Expand path
    char expanded_path[MAX_FILEPATH];
    expand_path(path, expanded_path);

    // Check if directory exists
    struct stat st;
    if (stat(expanded_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        // Directory doesn't exist, return empty list
        send(client_socket, "", 0, 0);
        return 0;
    }

    // Get file list
    char file_list[BUFFER_SIZE];
    memset(file_list, 0, BUFFER_SIZE);
    list_files_in_directory(expanded_path, extension, file_list);

    // Send file list
    send(client_socket, file_list, strlen(file_list), 0);
    
    return 0;
}

// Handle CREATE_TAR command (create tar of zip files, called from downltar)
int handle_create_tar_command(char *command, int client_socket) {
    char filetype[10];
    char response[BUFFER_SIZE];
    char tar_filename[MAX_FILENAME] = "zip.tar";
    
    // Parse command
    if (sscanf(command, "CREATE_TAR %s", filetype) != 1) {
        snprintf(response, BUFFER_SIZE, "ERROR: Invalid CREATE_TAR command syntax");
        send(client_socket, response, strlen(response), 0);
        return -1;
    }

    // Validate file type
    if (strcmp(filetype, "zip") != 0) {
        snprintf(response, BUFFER_SIZE, "ERROR: S4 only handles zip files");
        send(client_socket, response, strlen(response), 0);
        return -1;
    }

    // Create tar command - find all .zip files in S4 directory tree and create a tar
    char tar_command[COMMAND_SIZE];
    char s4_dir[MAX_FILEPATH];
    expand_path(S4_BASE_DIR, s4_dir);
    
    snprintf(tar_command, COMMAND_SIZE, 
             "find %s -name \"*.zip\" -type f -print0 | tar -cvf %s --null -T -", 
             s4_dir, tar_filename);
    
    // Execute tar command
    if (system(tar_command) != 0) {
        snprintf(response, BUFFER_SIZE, "ERROR: Failed to create tar file");
        send(client_socket, response, strlen(response), 0);
        return -1;
    }

    // Verify tar file exists
    if (access(tar_filename, F_OK) != 0) {
        snprintf(response, BUFFER_SIZE, "ERROR: Failed to create tar file");
        send(client_socket, response, strlen(response), 0);
        return -1;
    }

    // Send ready signal to S1
    snprintf(response, BUFFER_SIZE, "READY_TO_SEND_TAR %s", tar_filename);
    send(client_socket, response, strlen(response), 0);

    // Wait for acknowledgment
    memset(response, 0, BUFFER_SIZE);
    if (recv(client_socket, response, BUFFER_SIZE - 1, 0) <= 0 || 
        strcmp(response, "READY") != 0) {
        remove(tar_filename);
        return -1;
    }

    // Send tar file to S1
    int result = send_file(tar_filename, client_socket);
    
    // Delete temporary tar file
    if (remove(tar_filename) != 0) {
        perror("Warning: Failed to delete temporary tar file");
    }
    
    return result;
}

// Function to send file to socket
int send_file(const char *filepath, int client_socket) {
    FILE *fp = fopen(filepath, "rb");
    if (!fp) {
        perror("Error opening file for sending");
        return -1;
    }
    
    char buffer[BUFFER_SIZE];
    size_t bytes_read;
    
    while ((bytes_read = fread(buffer, 1, BUFFER_SIZE, fp)) > 0) {
        if (send(client_socket, buffer, bytes_read, 0) < 0) {
            perror("Error sending file data");
            fclose(fp);
            return -1;
        }
    }
    
    fclose(fp);
    return 0;
}

// Function to receive file from socket
int receive_file(const char *filepath, int client_socket) {
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
        perror("Error creating file for receiving");
        return -1;
    }
    
    char buffer[BUFFER_SIZE];
    ssize_t bytes_read;
    
    while ((bytes_read = recv(client_socket, buffer, BUFFER_SIZE, 0)) > 0) {
        if (fwrite(buffer, 1, bytes_read, fp) != bytes_read) {
            perror("Error writing to file");
            fclose(fp);
            return -1;
        }
        
        // Check if end of file (partial buffer means end of transfer)
        if (bytes_read < BUFFER_SIZE) {
            break;
        }
    }
    
    fclose(fp);
    
    if (bytes_read < 0) {
        perror("Error receiving file data");
        return -1;
    }
    
    return 0;
}

// Create directory path recursively
int create_directory_path(const char *path) {
    char tmp[MAX_FILEPATH];
    char *p = NULL;
    size_t len;
    
    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    
    // Remove trailing slash if present
    if (len > 0 && tmp[len - 1] == '/') {
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

// Expand path (replace ~ with home directory)
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

// Get file extension
char* get_file_extension(const char *filename) {
    char *dot = strrchr(filename, '.');
    if (!dot || dot == filename) {
        return NULL;
    }
    return dot + 1;
}

// List files in directory with specific extension
int list_files_in_directory(const char *path, const char *extension, char *file_list) {
    DIR *dir = opendir(path);
    if (!dir) {
        return -1;
    }
    
    struct dirent *entry;
    struct dirent **namelist;
    int n, count = 0;
    
    // Count number of matching files
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG) {
            char *ext = get_file_extension(entry->d_name);
            if (ext && strcmp(ext, extension) == 0) {
                count++;
            }
        }
    }
    rewinddir(dir);
    
    // Allocate array for entries
    char **files = malloc(count * sizeof(char *));
    if (!files) {
        closedir(dir);
        return -1;
    }
    
    // Fill array with filenames
    int i = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG) {
            char *ext = get_file_extension(entry->d_name);
            if (ext && strcmp(ext, extension) == 0 && i < count) {
                files[i] = strdup(entry->d_name);
                i++;
            }
        }
    }
    closedir(dir);
    
    // Sort files alphabetically
    for (int j = 0; j < i - 1; j++) {
        for (int k = j + 1; k < i; k++) {
            if (strcmp(files[j], files[k]) > 0) {
                char *temp = files[j];
                files[j] = files[k];
                files[k] = temp;
            }
        }
    }
    
    // Build file list string
    file_list[0] = '\0';
    for (int j = 0; j < i; j++) {
        strcat(file_list, files[j]);
        strcat(file_list, "\n");
        free(files[j]);
    }
    
    free(files);
    return 0;
}
