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
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == -1) {
        std::cerr << "socket error\n";
        return 1;
    }

    sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr);

    if (connect(sock, (sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
        std::cerr << "connect error\n";
        close(sock);
        return 1;
    }

    std::cout << "connected\n";

    Message msg;
    std::cout << "nickname: ";
    std::cin.getline(msg.payload, MAX_PAYLOAD);
    msg.type = MSG_HELLO;
    msg.length = htonl(strlen(msg.payload) + 1);
    send(sock, &msg.length, 4, 0);
    send(sock, &msg.type, 1, 0);
    send(sock, msg.payload, strlen(msg.payload), 0);

    recv(sock, &msg.length, 4, 0);
    recv(sock, &msg.type, 1, 0);
    recv(sock, msg.payload, ntohl(msg.length) - 1, 0);
    msg.payload[ntohl(msg.length) - 1] = 0;
    if (msg.type == MSG_WELCOME) {
        std::cout << msg.payload << "\n";
    }

    char input[256];
    while (true) {
        std::cout << "> ";
        std::cin.getline(input, sizeof(input));

        if (strcmp(input, "/quit") == 0) {
            msg.type = MSG_BYE;
            strcpy(msg.payload, "bye");
            msg.length = htonl(strlen(msg.payload) + 1);
            send(sock, &msg.length, 4, 0);
            send(sock, &msg.type, 1, 0);
            send(sock, msg.payload, strlen(msg.payload), 0);
            break;
        }
        else if (strcmp(input, "/ping") == 0) {
            msg.type = MSG_PING;
            strcpy(msg.payload, "ping");
            msg.length = htonl(strlen(msg.payload) + 1);
            send(sock, &msg.length, 4, 0);
            send(sock, &msg.type, 1, 0);
            send(sock, msg.payload, strlen(msg.payload), 0);
        }
        else {
            msg.type = MSG_TEXT;
            strcpy(msg.payload, input);
            msg.length = htonl(strlen(msg.payload) + 1);
            send(sock, &msg.length, 4, 0);
            send(sock, &msg.type, 1, 0);
            send(sock, msg.payload, strlen(msg.payload), 0);
        }

        int received = recv(sock, &msg.length, 4, 0);
        if (received <= 0) {
            std::cout << "server closed\n";
            break;
        }
        recv(sock, &msg.type, 1, 0);
        recv(sock, msg.payload, ntohl(msg.length) - 1, 0);
        msg.payload[ntohl(msg.length) - 1] = 0;

        if (msg.type == MSG_TEXT) {
            std::cout << msg.payload << "\n";
        }
        else if (msg.type == MSG_PONG) {
            std::cout << "PONG\n";
        }
        else if (msg.type == MSG_BYE) {
            std::cout << "server disconnected\n";
            break;
        }
    }

    close(sock);
    return 0;
}