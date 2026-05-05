#include <iostream>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <vector>
#include <queue>
#include <map>
#include <stdint.h>

#define MAX_PAYLOAD 1024
#define PORT 8888
#define THREAD_POOL_SIZE 10

using namespace std;

enum {
    MSG_HELLO = 1,
    MSG_WELCOME = 2,
    MSG_TEXT = 3,
    MSG_PING = 4,
    MSG_PONG = 5,
    MSG_BYE = 6,
    MSG_AUTH = 7,
    MSG_PRIVATE = 8,
    MSG_ERROR = 9,
    MSG_SERVER_INFO = 10
};

typedef struct {
    uint32_t length;
    uint8_t type;
    char payload[MAX_PAYLOAD];
} Message;

struct ClientInfo {
    int socket;
    char ip[INET_ADDRSTRLEN];
    int port;
    char nickname[32];
    int authenticated;
};

queue<int> client_queue;
vector<ClientInfo> clients;
pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t queue_cond = PTHREAD_COND_INITIALIZER;
bool server_running = true;

int send_message(int socket, uint8_t type, const char* payload) {
    Message msg;
    msg.type = type;
    strncpy(msg.payload, payload, MAX_PAYLOAD - 1);
    msg.payload[MAX_PAYLOAD - 1] = '\0';
    msg.length = htonl(strlen(msg.payload) + 1);
    
    if (send(socket, &msg.length, 4, 0) != 4) return -1;
    if (send(socket, &msg.type, 1, 0) != 1) return -1;
    if (send(socket, msg.payload, strlen(msg.payload), 0) != (ssize_t)strlen(msg.payload)) return -1;
    return 0;
}

int receive_message(int socket, Message& msg) {
    int bytes = recv(socket, &msg.length, 4, 0);
    if (bytes <= 0) return -1;
    
    bytes = recv(socket, &msg.type, 1, 0);
    if (bytes <= 0) return -1;
    
    uint32_t len = ntohl(msg.length);
    bytes = recv(socket, msg.payload, len - 1, 0);
    if (bytes <= 0) return -1;
    msg.payload[bytes] = '\0';
    
    return 0;
}

ClientInfo* find_client_by_nickname(const char* nickname) {
    for (size_t i = 0; i < clients.size(); i++) {
        if (clients[i].authenticated && strcmp(clients[i].nickname, nickname) == 0) {
            return &clients[i];
        }
    }
    return nullptr;
}

int send_private_message(const char* sender_nick, const char* target_nick, const char* message) {
    ClientInfo* target = find_client_by_nickname(target_nick);
    if (!target) return -1;
    
    char formatted_msg[1024];
    snprintf(formatted_msg, sizeof(formatted_msg), "[PRIVATE][%s]: %s", sender_nick, message);
    return send_message(target->socket, MSG_PRIVATE, formatted_msg);
}

void broadcast_message(const char* message, int sender_socket, const char* sender_nick) {
    char formatted_msg[1024];
    snprintf(formatted_msg, sizeof(formatted_msg), "[%s]: %s", sender_nick, message);
    
    pthread_mutex_lock(&clients_mutex);
    for (size_t i = 0; i < clients.size(); i++) {
        if (clients[i].authenticated && clients[i].socket != sender_socket) {
            send_message(clients[i].socket, MSG_TEXT, formatted_msg);
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

void broadcast_system_message(const char* message) {
    pthread_mutex_lock(&clients_mutex);
    for (size_t i = 0; i < clients.size(); i++) {
        if (clients[i].authenticated) {
            send_message(clients[i].socket, MSG_SERVER_INFO, message);
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

void remove_client(int client_socket) {
    pthread_mutex_lock(&clients_mutex);
    for (size_t i = 0; i < clients.size(); i++) {
        if (clients[i].socket == client_socket) {
            if (clients[i].authenticated) {
                char sys_msg[256];
                snprintf(sys_msg, sizeof(sys_msg), "User %s disconnected", clients[i].nickname);
                cout << sys_msg << endl;
                broadcast_system_message(sys_msg);
            }
            clients.erase(clients.begin() + i);
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    close(client_socket);
}

void handle_authenticated_message(ClientInfo& client, Message& msg) {
    switch (msg.type) {
        case MSG_TEXT:
            cout << "[" << client.nickname << "]: " << msg.payload << endl;
            broadcast_message(msg.payload, client.socket, client.nickname);
            break;
            
        case MSG_PRIVATE: {
            char* colon_pos = strchr(msg.payload, ':');
            if (colon_pos) {
                *colon_pos = '\0';
                char* target_nick = msg.payload;
                char* private_msg = colon_pos + 1;
                
                if (send_private_message(client.nickname, target_nick, private_msg) == 0) {
                    char confirm_msg[256];
                    snprintf(confirm_msg, sizeof(confirm_msg), "[SERVER]: Message sent to %s", target_nick);
                    send_message(client.socket, MSG_SERVER_INFO, confirm_msg);
                } else {
                    char error_msg[256];
                    snprintf(error_msg, sizeof(error_msg), "[SERVER]: User %s not found", target_nick);
                    send_message(client.socket, MSG_ERROR, error_msg);
                }
            }
            break;
        }
        
        case MSG_PING:
            send_message(client.socket, MSG_PONG, "pong");
            break;
            
        case MSG_BYE:
            break;
    }
}

void authenticate_client(int client_socket, ClientInfo& client) {
    Message auth_msg;
    receive_message(client_socket, auth_msg);
    
    if (auth_msg.type != MSG_AUTH || strlen(auth_msg.payload) == 0 || strlen(auth_msg.payload) >= 32) {
        send_message(client_socket, MSG_ERROR, "Authentication failed");
        return;
    }
    
    pthread_mutex_lock(&clients_mutex);
    for (size_t i = 0; i < clients.size(); i++) {
        if (clients[i].authenticated && strcmp(clients[i].nickname, auth_msg.payload) == 0) {
            pthread_mutex_unlock(&clients_mutex);
            send_message(client_socket, MSG_ERROR, "Nickname already taken");
            return;
        }
    }
    
    strcpy(client.nickname, auth_msg.payload);
    client.authenticated = 1;
    pthread_mutex_unlock(&clients_mutex);
    
    char welcome_msg[256];
    snprintf(welcome_msg, sizeof(welcome_msg), "Welcome, %s!", client.nickname);
    send_message(client_socket, MSG_SERVER_INFO, welcome_msg);
    
    char sys_msg[256];
    snprintf(sys_msg, sizeof(sys_msg), "User %s connected", client.nickname);
    cout << sys_msg << endl;
    broadcast_system_message(sys_msg);
}

void* worker_thread(void* arg) {
    while (server_running) {
        int client_socket = -1;
        
        pthread_mutex_lock(&queue_mutex);
        while (client_queue.empty() && server_running) {
            pthread_cond_wait(&queue_cond, &queue_mutex);
        }
        if (!client_queue.empty()) {
            client_socket = client_queue.front();
            client_queue.pop();
        }
        pthread_mutex_unlock(&queue_mutex);
        
        if (client_socket == -1) continue;
        
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        getpeername(client_socket, (struct sockaddr*)&client_addr, &addr_len);
        
        ClientInfo new_client;
        new_client.socket = client_socket;
        inet_ntop(AF_INET, &client_addr.sin_addr, new_client.ip, INET_ADDRSTRLEN);
        new_client.port = ntohs(client_addr.sin_port);
        new_client.authenticated = 0;
        strcpy(new_client.nickname, "");
        
        Message hello_msg;
        if (receive_message(client_socket, hello_msg) != 0 || hello_msg.type != MSG_HELLO) {
            close(client_socket);
            continue;
        }
        
        send_message(client_socket, MSG_WELCOME, "Welcome to chat server");
        
        authenticate_client(client_socket, new_client);
        if (!new_client.authenticated) {
            close(client_socket);
            continue;
        }
        
        pthread_mutex_lock(&clients_mutex);
        clients.push_back(new_client);
        pthread_mutex_unlock(&clients_mutex);
        
        while (server_running) {
            Message msg;
            if (receive_message(client_socket, msg) != 0) break;
            if (msg.type == MSG_BYE) break;
            handle_authenticated_message(new_client, msg);
        }
        
        remove_client(client_socket);
    }
    return NULL;
}

int main() {
    int server_fd;
    struct sockaddr_in server_addr;
    
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) return 1;
    
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);
    
    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) return 1;
    if (listen(server_fd, 10) < 0) return 1;
    
    cout << "Server started on port " << PORT << endl;
    
    pthread_t threads[THREAD_POOL_SIZE];
    for (int i = 0; i < THREAD_POOL_SIZE; i++) {
        pthread_create(&threads[i], NULL, worker_thread, NULL);
    }
    
    while (server_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) continue;
        
        pthread_mutex_lock(&queue_mutex);
        client_queue.push(client_fd);
        pthread_cond_signal(&queue_cond);
        pthread_mutex_unlock(&queue_mutex);
    }
    
    close(server_fd);
    return 0;
}
