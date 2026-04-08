#include <iostream>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <vector>
#include <queue>
#include <stdint.h>

#define MAX_DATA 1024
#define SERVER_PORT 8888
#define WORKER_COUNT 10

using namespace std;

typedef struct
{
    uint32_t packet_len;
    uint8_t packet_type;
    char packet_data[MAX_DATA];
} NetworkPacket;

enum
{
    TYPE_HELLO = 1,
    TYPE_WELCOME = 2,
    TYPE_MESSAGE = 3,
    TYPE_PING = 4,
    TYPE_PONG = 5,
    TYPE_BYE = 6
};

struct ClientEntry
{
    int socket_fd;
    char ip_address[INET_ADDRSTRLEN];
    int port_num;
    char user_name[100];
};

queue<int> pending_queue;
vector<ClientEntry> active_clients;
pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t clients_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t queue_condition = PTHREAD_COND_INITIALIZER;
bool server_active = true;

int transmit_packet(int socket_fd, uint8_t packet_type, const char* content)
{
    NetworkPacket packet;
    packet.packet_type = packet_type;
    strcpy(packet.packet_data, content);
    packet.packet_len = htonl(strlen(packet.packet_data) + 1);
    
    if (send(socket_fd, &packet.packet_len, 4, 0) != 4) return -1;
    if (send(socket_fd, &packet.packet_type, 1, 0) != 1) return -1;
    if (send(socket_fd, packet.packet_data, strlen(packet.packet_data), 0) != (ssize_t)strlen(packet.packet_data)) return -1;
    return 0;
}

int fetch_packet(int socket_fd, NetworkPacket& packet)
{
    int received = recv(socket_fd, &packet.packet_len, 4, 0);
    if (received <= 0) return -1;
    
    received = recv(socket_fd, &packet.packet_type, 1, 0);
    if (received <= 0) return -1;
    
    uint32_t length = ntohl(packet.packet_len);
    received = recv(socket_fd, packet.packet_data, length - 1, 0);
    if (received <= 0) return -1;
    packet.packet_data[received] = '\0';
    
    return 0;
}

void distribute_message(const char* message, int sender_socket)
{
    pthread_mutex_lock(&clients_lock);
    for (size_t i = 0; i < active_clients.size(); i++)
    {
        if (active_clients[i].socket_fd != sender_socket)
        {
            transmit_packet(active_clients[i].socket_fd, TYPE_MESSAGE, message);
        }
    }
    pthread_mutex_unlock(&clients_lock);
}

void disconnect_client(int client_socket)
{
    pthread_mutex_lock(&clients_lock);
    for (size_t i = 0; i < active_clients.size(); i++)
    {
        if (active_clients[i].socket_fd == client_socket)
        {
            cout << "Client disconnected: " << active_clients[i].user_name << " [" << active_clients[i].ip_address << ":" << active_clients[i].port_num << "]" << endl;
            active_clients.erase(active_clients.begin() + i);
            break;
        }
    }
    pthread_mutex_unlock(&clients_lock);
    close(client_socket);
}

void* process_worker(void* arg)
{
    while (server_active)
    {
        int client_socket = -1;
        
        pthread_mutex_lock(&queue_lock);
        while (pending_queue.empty() && server_active)
        {
            pthread_cond_wait(&queue_condition, &queue_lock);
        }
        if (!pending_queue.empty())
        {
            client_socket = pending_queue.front();
            pending_queue.pop();
        }
        pthread_mutex_unlock(&queue_lock);
        
        if (client_socket == -1) continue;
        
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        getpeername(client_socket, (struct sockaddr*)&client_addr, &addr_len);
        
        ClientEntry new_entry;
        new_entry.socket_fd = client_socket;
        inet_ntop(AF_INET, &client_addr.sin_addr, new_entry.ip_address, INET_ADDRSTRLEN);
        new_entry.port_num = ntohs(client_addr.sin_port);
        
        NetworkPacket hello_packet;
        if (fetch_packet(client_socket, hello_packet) != 0 || hello_packet.packet_type != TYPE_HELLO)
        {
            close(client_socket);
            continue;
        }
        
        strcpy(new_entry.user_name, hello_packet.packet_data);
        
        char welcome_text[200];
        sprintf(welcome_text, "Welcome %s:%d", new_entry.ip_address, new_entry.port_num);
        transmit_packet(client_socket, TYPE_WELCOME, welcome_text);
        
        pthread_mutex_lock(&clients_lock);
        active_clients.push_back(new_entry);
        pthread_mutex_unlock(&clients_lock);
        
        cout << "Client connected: " << new_entry.user_name << " [" << new_entry.ip_address << ":" << new_entry.port_num << "]" << endl;
        
        bool client_online = true;
        while (client_online && server_active)
        {
            NetworkPacket packet;
            int result = fetch_packet(client_socket, packet);
            
            if (result != 0)
            {
                client_online = false;
                break;
            }
            
            switch (packet.packet_type)
            {
                case TYPE_MESSAGE:
                    cout << "[" << new_entry.user_name << "]: " << packet.packet_data << endl;
                    distribute_message(packet.packet_data, client_socket);
                    break;
                    
                case TYPE_PING:
                    transmit_packet(client_socket, TYPE_PONG, "pong");
                    break;
                    
                case TYPE_BYE:
                    client_online = false;
                    break;
            }
        }
        
        disconnect_client(client_socket);
    }
    
    return NULL;
}

int main()
{
    int server_socket;
    struct sockaddr_in server_address;
    
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0)
    {
        cerr << "Failed to create socket" << endl;
        return 1;
    }
    
    int reuse_flag = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &reuse_flag, sizeof(reuse_flag));
    
    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = INADDR_ANY;
    server_address.sin_port = htons(SERVER_PORT);
    
    if (bind(server_socket, (struct sockaddr*)&server_address, sizeof(server_address)) < 0)
    {
        cerr << "Bind failed" << endl;
        close(server_socket);
        return 1;
    }
    
    if (listen(server_socket, 10) < 0)
    {
        cerr << "Listen failed" << endl;
        close(server_socket);
        return 1;
    }
    
    cout << "Server started on port " << SERVER_PORT << endl;
    cout << "Thread pool size: " << WORKER_COUNT << endl;
    
    pthread_t workers[WORKER_COUNT];
    for (int i = 0; i < WORKER_COUNT; i++)
    {
        pthread_create(&workers[i], NULL, process_worker, NULL);
    }
    
    while (server_active)
    {
        struct sockaddr_in client_address;
        socklen_t client_length = sizeof(client_address);
        int client_fd = accept(server_socket, (struct sockaddr*)&client_address, &client_length);
        
        if (client_fd < 0)
        {
            cerr << "Accept failed" << endl;
            continue;
        }
        
        pthread_mutex_lock(&queue_lock);
        pending_queue.push(client_fd);
        pthread_cond_signal(&queue_condition);
        pthread_mutex_unlock(&queue_lock);
    }
    
    close(server_socket);
    return 0;
}