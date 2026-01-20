#include <iostream>
#include <thread>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <string>
#include <algorithm>
#include <chrono>

#pragma comment(lib, "ws2_32.lib")
using namespace std;
class TcpClient {
public:
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    void connectToServer(const std::string& ip, uint16_t port) {
        sockaddr_in server{};
        server.sin_family = AF_INET;
        server.sin_port = htons(port);
        inet_pton(AF_INET, ip.c_str(), &server.sin_addr);
        while (true) {
            if (connect(sock, (sockaddr*)&server, sizeof(server)) != 0) {
                std::cout << "Connect failed: " << WSAGetLastError() << "\n";
                std::this_thread::sleep_for(std::chrono::seconds(5));
                if (connect(sock, (sockaddr*)&server, sizeof(server)) == 0) {
                    std::cout << "Connected!\n";
                    break;

                }
            }
            else {
                std::cout << "Connected!\n";
                break;
			}
        }

        
    }

    std::string receiveMessages() {
        uint32_t len;

        int r = recv(sock, (char*)&len, sizeof(len), 0);
        if (r <= 0) return "";

        len = ntohl(len);

        std::string msg;
        msg.reserve(len);

        char buffer[2048];
        int received = 0;

        while (received < (int)len) {
            int bytes = recv(sock, buffer,
                min((int)sizeof(buffer), (int)len - received), 0);

            if (bytes <= 0) return "";

            msg.append(buffer, bytes);
            received += bytes;
        }

        return msg;
    }


    void sendMessage(std::string msg) {
        if (!msg.empty()) {           
            uint32_t nlen = htonl((uint32_t)msg.size());
            send(sock, (char*)&nlen, sizeof(nlen), 0);
            send(sock, msg.data(), (int)msg.size(), 0);
        }
    }

    void closeConnection() {
        closesocket(sock);
        WSACleanup();
    }
};
int main() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    TcpClient client;
    SOCKET sock = client.sock;

    client.connectToServer("26.222.92.86", 8080);

    std::thread receiver([&]() {
        while (true) {
            std::string msg = client.receiveMessages();
            if (!msg.empty()) {
                std::cout << "Freund: " << msg << "\n";
            }
        }
        });

    std::thread sender([&] {
        std::string msg;
        while (std::getline(std::cin, msg)) {
            client.sendMessage(msg);
        }
        });

    receiver.join();
    sender.join();

    client.closeConnection();

    return 0;
}
