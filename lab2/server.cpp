#include <iostream>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <stdint.h>

#define MAX_PAYLOAD 1024
#define PORT 8888

struct Message {
    uint32_t length;
    uint8_t type;
    char payload[MAX_PAYLOAD];
};

enum {
    MSG_HELLO = 1,
    MSG_WELCOME = 2,
    MSG_TEXT = 3,
    MSG_PING = 4,
    MSG_PONG = 5,
    MSG_BYE = 6
};

int main() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        std::cerr << "socket error\n";
        return 1;
    }

    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);

    if (bind(server_fd, (sockaddr*)&addr, sizeof(addr)) == -1) {
        std::cerr << "bind error\n";
        close(server_fd);
        return 1;
    }

    if (listen(server_fd, 1) == -1) {
        std::cerr << "listen error\n";
        close(server_fd);
        return 1;
    }

    std::cout << "server on port " << PORT << "\n";

    sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int client_fd = accept(server_fd, (sockaddr*)&client_addr, &client_len);
    if (client_fd == -1) {
        std::cerr << "accept error\n";
        close(server_fd);
        return 1;
    }

    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
    std::cout << "client connected\n";

    Message msg;
    recv(client_fd, &msg.length, 4, 0);
    recv(client_fd, &msg.type, 1, 0);
    recv(client_fd, msg.payload, ntohl(msg.length) - 1, 0);
    msg.payload[ntohl(msg.length) - 1] = 0;

    if (msg.type == MSG_HELLO) {
        std::cout << "[" << client_ip << "] hello: " << msg.payload << "\n";

        msg.type = MSG_WELCOME;
        sprintf(msg.payload, "welcome %s", client_ip);
        msg.length = htonl(strlen(msg.payload) + 1);
        send(client_fd, &msg.length, 4, 0);
        send(client_fd, &msg.type, 1, 0);
        send(client_fd, msg.payload, strlen(msg.payload), 0);
    }

    while (true) {
        int received = recv(client_fd, &msg.length, 4, 0);
        if (received <= 0) {
            std::cout << "client disconnected\n";
            break;
        }

        recv(client_fd, &msg.type, 1, 0);
        recv(client_fd, msg.payload, ntohl(msg.length) - 1, 0);
        msg.payload[ntohl(msg.length) - 1] = 0;

        if (msg.type == MSG_TEXT) {
            std::cout << "[" << client_ip << "] " << msg.payload << "\n";
        }
        else if (msg.type == MSG_PING) {
            msg.type = MSG_PONG;
            strcpy(msg.payload, "pong");
            msg.length = htonl(strlen(msg.payload) + 1);
            send(client_fd, &msg.length, 4, 0);
            send(client_fd, &msg.type, 1, 0);
            send(client_fd, msg.payload, strlen(msg.payload), 0);
        }
        else if (msg.type == MSG_BYE) {
            std::cout << "client disconnected\n";
            break;
        }
    }

    close(client_fd);
    close(server_fd);
    return 0;
}