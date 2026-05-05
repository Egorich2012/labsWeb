#include <iostream>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <stdint.h>

#define MAX_PAYLOAD 1024
#define PORT 8888
#define RECONNECT_DELAY 2

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

int sock = -1;
bool connected = false;
char my_nickname[32];
pthread_mutex_t socket_mutex = PTHREAD_MUTEX_INITIALIZER;

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
    if (recv(socket, &msg.length, 4, 0) <= 0) return -1;
    if (recv(socket, &msg.type, 1, 0) <= 0) return -1;
    
    uint32_t len = ntohl(msg.length);
    int bytes = recv(socket, msg.payload, len - 1, 0);
    if (bytes <= 0) return -1;
    msg.payload[bytes] = '\0';
    return 0;
}

bool connect_to_server() {
    int new_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (new_sock < 0) return false;
    
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr);
    
    if (connect(new_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        close(new_sock);
        return false;
    }
    
    Message hello_msg = {htonl(strlen("Client") + 1), MSG_HELLO, "Client"};
    send(new_sock, &hello_msg.length, 4, 0);
    send(new_sock, &hello_msg.type, 1, 0);
    send(new_sock, hello_msg.payload, strlen(hello_msg.payload), 0);
    
    Message welcome_msg;
    if (receive_message(new_sock, welcome_msg) != 0 || welcome_msg.type != MSG_WELCOME) {
        close(new_sock);
        return false;
    }
    cout << welcome_msg.payload << endl;
    
    cout << "Enter your nickname: ";
    cin.getline(my_nickname, 32);
    
    if (send_message(new_sock, MSG_AUTH, my_nickname) != 0) {
        close(new_sock);
        return false;
    }
    
    Message auth_response;
    if (receive_message(new_sock, auth_response) != 0) {
        close(new_sock);
        return false;
    }
    
    if (auth_response.type == MSG_ERROR) {
        cout << "Authentication failed: " << auth_response.payload << endl;
        close(new_sock);
        return false;
    }
    if (auth_response.type == MSG_SERVER_INFO) {
        cout << auth_response.payload << endl;
    }
    
    pthread_mutex_lock(&socket_mutex);
    sock = new_sock;
    connected = true;
    pthread_mutex_unlock(&socket_mutex);
    return true;
}

void* receive_thread(void* arg) {
    while (true) {
        pthread_mutex_lock(&socket_mutex);
        int current_sock = sock;
        bool is_connected = connected;
        pthread_mutex_unlock(&socket_mutex);
        
        if (!is_connected || current_sock == -1) break;
        
        Message msg;
        if (receive_message(current_sock, msg) != 0) {
            pthread_mutex_lock(&socket_mutex);
            close(sock);
            sock = -1;
            connected = false;
            pthread_mutex_unlock(&socket_mutex);
            cout << "\nConnection lost. Reconnecting..." << endl;
            break;
        }
        
        if (msg.type == MSG_TEXT || msg.type == MSG_PRIVATE || 
            msg.type == MSG_PONG || msg.type == MSG_SERVER_INFO || msg.type == MSG_ERROR) {
            const char* label = (msg.type == MSG_PONG) ? "\nPONG received" : 
                               (msg.type == MSG_ERROR) ? "\n[ERROR]: " : "\n";
            if (msg.type == MSG_ERROR) cout << label << msg.payload;
            else if (msg.type == MSG_PONG) cout << label;
            else cout << "\n" << msg.payload;
            cout << "\n> " << flush;
        }
    }
    return NULL;
}

int main() {
    char input[256];
    pthread_t recv_thread;
    bool should_exit = false;
    
    while (!should_exit) {
        if (!connected) {
            if (connect_to_server()) {
                pthread_create(&recv_thread, NULL, receive_thread, NULL);
                pthread_detach(recv_thread);
                cout << "\nCommands: /quit, /ping, /w <nick> <message>\n> " << flush;
            } else {
                cout << "Connection failed. Retrying in " << RECONNECT_DELAY << " seconds..." << endl;
                sleep(RECONNECT_DELAY);
                continue;
            }
        }
        
        cin.getline(input, 256);
        
        pthread_mutex_lock(&socket_mutex);
        int current_sock = sock;
        bool is_connected = connected;
        pthread_mutex_unlock(&socket_mutex);
        
        if (!is_connected || current_sock == -1) continue;
        
        if (strcmp(input, "/quit") == 0) {
            send_message(current_sock, MSG_BYE, "bye");
            pthread_mutex_lock(&socket_mutex);
            close(sock);
            sock = -1;
            connected = false;
            pthread_mutex_unlock(&socket_mutex);
            should_exit = true;
            cout << "Disconnected" << endl;
        }
        else if (strcmp(input, "/ping") == 0) {
            send_message(current_sock, MSG_PING, "ping");
            cout << "PING sent\n> " << flush;
        }
        else if (strncmp(input, "/w ", 3) == 0) {
            char* space = strchr(input + 3, ' ');
            if (space) {
                *space = '\0';
                char private_payload[512];
                snprintf(private_payload, sizeof(private_payload), "%s:%s", input + 3, space + 1);
                send_message(current_sock, MSG_PRIVATE, private_payload);
                cout << "> " << flush;
            } else {
                cout << "Usage: /w <nickname> <message>\n> " << flush;
            }
        }
        else if (strlen(input) > 0) {
            send_message(current_sock, MSG_TEXT, input);
            cout << "> " << flush;
        }
        else {
            cout << "> " << flush;
        }
    }
    return 0;
}
