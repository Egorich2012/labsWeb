#include <iostream>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <stdint.h>

#define BUFFER_SIZE 1024
#define SERVER_PORT 8888
#define RETRY_DELAY 2

using namespace std;

typedef struct
{
    uint32_t data_len;
    uint8_t msg_type;
    char msg_data[BUFFER_SIZE];
} NetworkMessage;

enum
{
    TYPE_HELLO = 1,
    TYPE_WELCOME = 2,
    TYPE_MESSAGE = 3,
    TYPE_PING = 4,
    TYPE_PONG = 5,
    TYPE_BYE = 6
};

int client_socket = -1;
bool is_connected = false;
pthread_mutex_t mutex_socket = PTHREAD_MUTEX_INITIALIZER;

int transmit_message(int socket_fd, uint8_t msg_type, const char* content)
{
    NetworkMessage packet;
    packet.msg_type = msg_type;
    strcpy(packet.msg_data, content);
    packet.data_len = htonl(strlen(packet.msg_data) + 1);
    
    if (send(socket_fd, &packet.data_len, 4, 0) != 4) return -1;
    if (send(socket_fd, &packet.msg_type, 1, 0) != 1) return -1;
    if (send(socket_fd, packet.msg_data, strlen(packet.msg_data), 0) != (ssize_t)strlen(packet.msg_data)) return -1;
    return 0;
}

int fetch_message(int socket_fd, NetworkMessage& packet)
{
    int received = recv(socket_fd, &packet.data_len, 4, 0);
    if (received <= 0) return -1;
    
    received = recv(socket_fd, &packet.msg_type, 1, 0);
    if (received <= 0) return -1;
    
    uint32_t length = ntohl(packet.data_len);
    received = recv(socket_fd, packet.msg_data, length - 1, 0);
    if (received <= 0) return -1;
    packet.msg_data[received] = '\0';
    
    return 0;
}

bool establish_connection()
{
    int new_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (new_socket < 0) return false;
    
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    
    if (connect(new_socket, (struct sockaddr*)&addr, sizeof(addr)) < 0)
    {
        close(new_socket);
        return false;
    }
    
    NetworkMessage hello_packet;
    hello_packet.msg_type = TYPE_HELLO;
    strcpy(hello_packet.msg_data, "Client");
    hello_packet.data_len = htonl(strlen(hello_packet.msg_data) + 1);
    
    send(new_socket, &hello_packet.data_len, 4, 0);
    send(new_socket, &hello_packet.msg_type, 1, 0);
    send(new_socket, hello_packet.msg_data, strlen(hello_packet.msg_data), 0);
    
    NetworkMessage welcome_packet;
    if (fetch_message(new_socket, welcome_packet) != 0 || welcome_packet.msg_type != TYPE_WELCOME)
    {
        close(new_socket);
        return false;
    }
    
    cout << welcome_packet.msg_data << endl;
    
    pthread_mutex_lock(&mutex_socket);
    client_socket = new_socket;
    is_connected = true;
    pthread_mutex_unlock(&mutex_socket);
    
    return true;
}

void* listener_thread(void* arg)
{
    while (true)
    {
        pthread_mutex_lock(&mutex_socket);
        int current_socket = client_socket;
        bool connected_status = is_connected;
        pthread_mutex_unlock(&mutex_socket);
        
        if (!connected_status || current_socket == -1) break;
        
        NetworkMessage packet;
        int result = fetch_message(current_socket, packet);
        
        if (result != 0)
        {
            pthread_mutex_lock(&mutex_socket);
            close(client_socket);
            client_socket = -1;
            is_connected = false;
            pthread_mutex_unlock(&mutex_socket);
            cout << "\nConnection lost. Reconnecting..." << endl;
            break;
        }
        
        if (packet.msg_type == TYPE_MESSAGE)
        {
            cout << "\n[Broadcast] " << packet.msg_data << endl;
            cout << "> " << flush;
        }
        else if (packet.msg_type == TYPE_PONG)
        {
            cout << "\nPONG" << endl;
            cout << "> " << flush;
        }
    }
    return NULL;
}

int main()
{
    char user_input[100];
    pthread_t listener;
    bool exit_flag = false;
    
    while (!exit_flag)
    {
        if (!is_connected)
        {
            if (establish_connection())
            {
                pthread_create(&listener, NULL, listener_thread, NULL);
                pthread_detach(listener);
                cout << "Type /quit to disconnect, /ping for ping" << endl;
                cout << "> " << flush;
            }
            else
            {
                cout << "Connection failed. Retrying in " << RETRY_DELAY << " seconds..." << endl;
                sleep(RETRY_DELAY);
                continue;
            }
        }
        
        cin.getline(user_input, 100);
        
        pthread_mutex_lock(&mutex_socket);
        int current_socket = client_socket;
        bool connected_status = is_connected;
        pthread_mutex_unlock(&mutex_socket);
        
        if (!connected_status || current_socket == -1)
        {
            continue;
        }
        
        if (strcmp(user_input, "/quit") == 0)
        {
            transmit_message(current_socket, TYPE_BYE, "bye");
            pthread_mutex_lock(&mutex_socket);
            close(client_socket);
            client_socket = -1;
            is_connected = false;
            pthread_mutex_unlock(&mutex_socket);
            exit_flag = true;
        }
        else if (strcmp(user_input, "/ping") == 0)
        {
            transmit_message(current_socket, TYPE_PING, "ping");
            cout << "> " << flush;
        }
        else if (strlen(user_input) > 0)
        {
            transmit_message(current_socket, TYPE_MESSAGE, user_input);
            cout << "> " << flush;
        }
        else
        {
            cout << "> " << flush;
        }
    }
    
    cout << "Disconnected" << endl;
    return 0;
}