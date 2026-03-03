#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <iostream>

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <IP> <port>\n";
        return 1;
    }

    int udp_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_socket == -1) {
        std::cerr << "socket creation error\n";
        return 1;
    }

    sockaddr_in server_info;
    server_info.sin_family = AF_INET;
    server_info.sin_port = htons(std::atoi(argv[2]));
    if (inet_pton(AF_INET, argv[1], &server_info.sin_addr) <= 0) {
        std::cerr << "invalid address\n";
        close(udp_socket);
        return 1;
    }

    std::string user_input;
    char reply_buffer[2048];

    while (true) {
        std::cout << "> ";
        std::getline(std::cin, user_input);

        sendto(udp_socket, user_input.c_str(), user_input.length(), 0,
               (sockaddr*)&server_info, sizeof(server_info));

        sockaddr_in sender_info;
        socklen_t sender_size = sizeof(sender_info);
        int bytes_received = recvfrom(udp_socket, reply_buffer, sizeof(reply_buffer) - 1, 0,
                                      (sockaddr*)&sender_info, &sender_size);
        if (bytes_received == -1) {
            std::cerr << "receive error\n";
            continue;
        }
        reply_buffer[bytes_received] = '\0';

        std::cout << "server replied: " << reply_buffer << "\n";
    }

    close(udp_socket);
    return 0;
}