# Distributed-File-System
Distributed File System through socket programming

# Distributed File System (Project - Summer 2025)

## Overview

This project implements a **Distributed File System** using **socket programming in C** as part of the COMP-8567 course. It simulates a client-server model where clients interact with a single main server (S1), which distributes files in the background to three other servers (S2, S3, S4) based on file type. Clients are unaware of this distribution and assume all files are stored on S1.

## Architecture

- **S1 (Main Server):** Handles all client connections. Stores '.c' files and redirects '.pdf', '.txt', and '.zip' files to respective secondary servers.
- **S2 (PDF Server):** Stores '.pdf' files received from S1.
- **S3 (Text Server):** Stores '.txt' files received from S1.
- **S4 (Zip Server):** Stores '.zip' files received from S1.
- **Client (w25clients):** Sends upload, download, remove, display, and tar commands to S1. It never communicates directly with S2, S3, or S4.

All servers and clients run on separate terminals and communicate strictly via sockets.

## Features

The client supports the following commands:

#### 1. 'uploadf filename destination_path'
Uploads a file to the S1 server. Based on file extension:
- '.c' is stored in S1.
- '.pdf' is transferred to S2.
- '.txt' is transferred to S3.
- '.zip' is transferred to S4.

**Example:**
In bash
uploadf sample.c ~S1/folder1/folder2

Like the **uploadf command**, the following commands follow a similar structure.

- downlf ~S1/folder1/folder2/sample.txt
- removef ~S1/folder1/folder2/sample.pdf
- downltar .txt
- dispfnames ~S1/folder1


#### **How to Compile**
Use gcc to compile each file:

In bash
- gcc -o S1 S1.c
- gcc -o S2 S2.c
- gcc -o S3 S3.c
- gcc -o S4 S4.c
- gcc -o w25clients w25clients.c

**Assumptions**
- All client communication is via S1; 
- S2–S4 do not interact with clients.
- Directory structure under ~/S1 must be mirrored in S2–S4.
- Clients must only use .c, .pdf, .txt, or .zip files.
- All files are moved from S1 to their respective backend servers after upload (if not .c).


#### Sample Output Screenshots
- Client Side
 
![image](https://github.com/user-attachments/assets/39eca2a7-8b7f-44aa-a46c-2e3648459a8f)

- S1 Server
  
![image](https://github.com/user-attachments/assets/cf10b1cd-8d00-49ca-8b19-4d76a2b59cc9)

- S2 Server
  
![image](https://github.com/user-attachments/assets/46637e5c-46ba-41f7-a552-1de3a714d6dc)

- S3 Server
  
![image](https://github.com/user-attachments/assets/1a1501d7-e4be-45ff-b1ee-090bf3481f2c)

- S4 Server
  
![image](https://github.com/user-attachments/assets/7c742548-e18c-4721-b872-de20c8ad129a)




