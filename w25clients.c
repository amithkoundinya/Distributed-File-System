#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <fcntl.h>
#include <errno.h>
#include <libgen.h>
#include <sys/stat.h>

#define BUFFER_SIZE 4096
#define CMD_SIZE 1024
#define MAX_PATH 1024
#define S1_IP "127.0.0.1"
#define S1_PORT 8386

/* Function to validate if a file exists in current directory */
int validate_file_existence(const char *filename) {
    struct stat file_stat;
    if (stat(filename, &file_stat) != 0) {
        return 0; // File doesn't exist
    }
    return S_ISREG(file_stat.st_mode); // Ensure it's a regular file
}

/* Function to validate file extension */
int validate_file_extension(const char *filename) {
    const char *dot = strrchr(filename, '.');
    if (!dot || dot == filename) {
        return 0; // No extension found
    }
    
    if (strcmp(dot + 1, "c") == 0 || 
        strcmp(dot + 1, "pdf") == 0 || 
        strcmp(dot + 1, "txt") == 0 || 
        strcmp(dot + 1, "zip") == 0) {
        return 1; // Valid extension
    }
    
    return 0; // Invalid extension
}

/* Function to validate if path starts with ~/S1 */
int validate_s1_path(const char *path) {
    if (strncmp(path, "~/S1", 4) == 0) {
        return 1;
    }
    return 0;
}

/* Function to validate file type for tar download */
int validate_tar_filetype(const char *filetype) {
    if (strcmp(filetype, "c") == 0 || 
        strcmp(filetype, "pdf") == 0 || 
        strcmp(filetype, "txt") == 0) {
        return 1;
    }
    return 0;
}

/* Function to send a file to the server */
int send_file_to_server(int sock, const char *filename) {
    FILE *file = fopen(filename, "rb");
    if (!file) {
        perror("Error opening file for upload");
        return -1;
    }
    
    char buffer[BUFFER_SIZE];
    size_t bytes_read;
    
    while ((bytes_read = fread(buffer, 1, BUFFER_SIZE, file)) > 0) {
        if (send(sock, buffer, bytes_read, 0) < 0) {
            perror("Error sending file data");
            fclose(file);
            return -1;
        }
    }
    
    fclose(file);
    return 0;
}

/* Function to receive a file from the server */
int receive_file_from_server(int sock, const char *filename) {
    FILE *file = fopen(filename, "wb");
    if (!file) {
        perror("Error creating file for download");
        return -1;
    }
    
    char buffer[BUFFER_SIZE];
    ssize_t bytes_received;
    
    while ((bytes_received = recv(sock, buffer, BUFFER_SIZE, 0)) > 0) {
        if (fwrite(buffer, 1, bytes_received, file) != bytes_received) {
            perror("Error writing received data to file");
            fclose(file);
            return -1;
        }
        
        // If we received less than the buffer size, we're done
        if (bytes_received < BUFFER_SIZE) {
            break;
        }
    }
    
    fclose(file);
    
    if (bytes_received < 0) {
        perror("Error receiving file data");
        return -1;
    }
    
    return 0;
}

/* Function to connect to S1 server */
int connect_to_s1_server() {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("Error creating socket");
        return -1;
    }
    
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(S1_PORT);
    
    if (inet_pton(AF_INET, S1_IP, &server_addr.sin_addr) <= 0) {
        perror("Invalid address or address not supported");
        close(sock);
        return -1;
    }
    
    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Connection to S1 server failed");
        close(sock);
        return -1;
    }
    
    return sock;
}

/* Function to handle uploadf command */
int handle_uploadf(int sock, const char *filename, const char *destination) {
    // Validate file exists
    if (!validate_file_existence(filename)) {
        printf("Error: File '%s' does not exist in current directory\n", filename);
        return -1;
    }
    
    // Validate file extension
    if (!validate_file_extension(filename)) {
        printf("Error: Only .c, .pdf, .txt, and .zip files are supported\n");
        return -1;
    }
    
    // Validate destination path
    if (!validate_s1_path(destination)) {
        printf("Error: Destination path must be within ~/S1\n");
        return -1;
    }
    
    // Send command to server
    char command[CMD_SIZE];
    snprintf(command, CMD_SIZE, "uploadf %s %s", basename((char*)filename), destination);
    
    if (send(sock, command, strlen(command), 0) < 0) {
        perror("Error sending command to server");
        return -1;
    }
    
    // Wait for server response
    char response[BUFFER_SIZE];
    memset(response, 0, BUFFER_SIZE);
    
    if (recv(sock, response, BUFFER_SIZE - 1, 0) <= 0) {
        perror("Error receiving response from server");
        return -1;
    }
    
    if (strcmp(response, "READY_TO_RECEIVE") != 0) {
        printf("%s\n", response);
        return -1;
    }
    
    // Send file to server
    if (send_file_to_server(sock, filename) != 0) {
        return -1;
    }
    
    // Get final response from server
    memset(response, 0, BUFFER_SIZE);
    if (recv(sock, response, BUFFER_SIZE - 1, 0) <= 0) {
        perror("Error receiving response from server");
        return -1;
    }
    
    printf("%s\n", response);
    return 0;
}

/* Function to handle downlf command */
int handle_downlf(int sock, const char *filepath) {
    // Validate path format
    if (!validate_s1_path(filepath)) {
        printf("Error: File path must be within ~/S1\n");
        return -1;
    }
    
    // Send command to server
    char command[CMD_SIZE];
    snprintf(command, CMD_SIZE, "downlf %s", filepath);
    
    if (send(sock, command, strlen(command), 0) < 0) {
        perror("Error sending command to server");
        return -1;
    }
    
    // Wait for server response
    char response[BUFFER_SIZE];
    memset(response, 0, BUFFER_SIZE);
    
    if (recv(sock, response, BUFFER_SIZE - 1, 0) <= 0) {
        perror("Error receiving response from server");
        return -1;
    }
    
    if (strcmp(response, "READY_TO_SEND") != 0) {
        printf("%s\n", response);
        return -1;
    }
    
    // Extract filename from path
    char *filename = basename((char*)filepath);
    
    // Receive file from server
    if (receive_file_from_server(sock, filename) != 0) {
        return -1;
    }
    
    printf("File '%s' downloaded successfully\n", filename);
    return 0;
}

/* Function to handle removef command */
int handle_removef(int sock, const char *filepath) {
    // Validate path format
    if (!validate_s1_path(filepath)) {
        printf("Error: File path must be within ~/S1\n");
        return -1;
    }
    
    // Send command to server
    char command[CMD_SIZE];
    snprintf(command, CMD_SIZE, "removef %s", filepath);
    
    if (send(sock, command, strlen(command), 0) < 0) {
        perror("Error sending command to server");
        return -1;
    }
    
    // Wait for server response
    char response[BUFFER_SIZE];
    memset(response, 0, BUFFER_SIZE);
    
    if (recv(sock, response, BUFFER_SIZE - 1, 0) <= 0) {
        perror("Error receiving response from server");
        return -1;
    }
    
    printf("%s\n", response);
    return 0;
}

/* Function to handle downltar command */
int handle_downltar(int sock, const char *filetype) {
    // Validate file type
    if (!validate_tar_filetype(filetype)) {
        printf("Error: Invalid file type. Only c, pdf, and txt are supported for tar download\n");
        return -1;
    }
    
    // Send command to server
    char command[CMD_SIZE];
    snprintf(command, CMD_SIZE, "downltar %s", filetype);
    
    if (send(sock, command, strlen(command), 0) < 0) {
        perror("Error sending command to server");
        return -1;
    }
    
    // Wait for server response
    char response[BUFFER_SIZE];
    memset(response, 0, BUFFER_SIZE);
    
    if (recv(sock, response, BUFFER_SIZE - 1, 0) <= 0) {
        perror("Error receiving response from server");
        return -1;
    }
    
    // Check if server is ready to send tar
    char tar_filename[256];
    if (sscanf(response, "READY_TO_SEND_TAR %s", tar_filename) != 1) {
        printf("%s\n", response);
        return -1;
    }
    
    // Send ready signal
    if (send(sock, "READY", 5, 0) < 0) {
        perror("Error sending ready signal");
        return -1;
    }
    
    // Receive tar file from server
    if (receive_file_from_server(sock, tar_filename) != 0) {
        return -1;
    }
    
    printf("Tar file '%s' downloaded successfully\n", tar_filename);
    return 0;
}

/* Function to handle dispfnames command */
int handle_dispfnames(int sock, const char *pathname) {
    // Validate path format
    if (!validate_s1_path(pathname)) {
        printf("Error: Path must be within ~/S1\n");
        return -1;
    }
    
    // Send command to server
    char command[CMD_SIZE];
    snprintf(command, CMD_SIZE, "dispfnames %s", pathname);
    
    if (send(sock, command, strlen(command), 0) < 0) {
        perror("Error sending command to server");
        return -1;
    }
    
    // Wait for server response
    char response[BUFFER_SIZE];
    memset(response, 0, BUFFER_SIZE);
    
    if (recv(sock, response, BUFFER_SIZE - 1, 0) <= 0) {
        perror("Error receiving response from server");
        return -1;
    }
    
    if (strcmp(response, "FILES_COMING") != 0) {
        printf("%s\n", response);
        return -1;
    }
    
    // Send ready signal
    if (send(sock, "READY", 5, 0) < 0) {
        perror("Error sending ready signal");
        return -1;
    }
    
    // Receive file list
    memset(response, 0, BUFFER_SIZE);
    if (recv(sock, response, BUFFER_SIZE - 1, 0) <= 0) {
        perror("Error receiving file list");
        return -1;
    }
    
    printf("Files in %s:\n%s\n", pathname, response);
    return 0;
}

int main() {
    char input[CMD_SIZE];
    char cmd[32];
    char arg1[MAX_PATH];
    char arg2[MAX_PATH];
    
    printf("W25 Distributed File System Client\n");
    printf("Available commands:\n");
    printf("  uploadf <filename> <destination_path>\n");
    printf("  downlf <filename>\n");
    printf("  removef <filename>\n");
    printf("  downltar <filetype>\n");
    printf("  dispfnames <pathname>\n");
    printf("  exit\n");
    
    while (1) {
        printf("\nw25clients$ ");
        fflush(stdout);
        
        // Get user input
        if (fgets(input, CMD_SIZE, stdin) == NULL) {
            break;
        }
        
        // Remove newline character
        input[strcspn(input, "\n")] = 0;
        
        // Check for exit command
        if (strcmp(input, "exit") == 0) {
            printf("Exiting client...\n");
            break;
        }
        
        // Parse command
        int args = sscanf(input, "%s %s %s", cmd, arg1, arg2);
        
        if (args < 1) {
            printf("Error: No command entered\n");
            continue;
        }
        
        // Connect to S1 server
        int sock = connect_to_s1_server();
        if (sock < 0) {
            printf("Failed to connect to S1 server\n");
            continue;
        }
        
        // Process command
        if (strcmp(cmd, "uploadf") == 0) {
            if (args != 3) {
                printf("Error: Usage: uploadf <filename> <destination_path>\n");
                close(sock);
                continue;
            }
            handle_uploadf(sock, arg1, arg2);
        } 
        else if (strcmp(cmd, "downlf") == 0) {
            if (args != 2) {
                printf("Error: Usage: downlf <filename>\n");
                close(sock);
                continue;
            }
            handle_downlf(sock, arg1);
        } 
        else if (strcmp(cmd, "removef") == 0) {
            if (args != 2) {
                printf("Error: Usage: removef <filename>\n");
                close(sock);
                continue;
            }
            handle_removef(sock, arg1);
        } 
        else if (strcmp(cmd, "downltar") == 0) {
            if (args != 2) {
                printf("Error: Usage: downltar <filetype>\n");
                close(sock);
                continue;
            }
            handle_downltar(sock, arg1);
        } 
        else if (strcmp(cmd, "dispfnames") == 0) {
            if (args != 2) {
                printf("Error: Usage: dispfnames <pathname>\n");
                close(sock);
                continue;
            }
            handle_dispfnames(sock, arg1);
        } 
        else {
            printf("Error: Unknown command '%s'\n", cmd);
        }
        
        // Close connection to server
        close(sock);
    }
    
    return 0;
}
