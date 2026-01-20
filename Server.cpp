/*

SERVER CODE

*/


#include <iostream>
#include <thread>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <string>
#include <algorithm>

#pragma comment(lib, "ws2_32.lib")

using namespace std;

enum PacketType : uint8_t {
    PT_USER = 1,
    PT_ACK = 2
};

struct Packet {
    uint8_t type = 0;
    uint32_t msgId = 0;
    std::string data;
};

class SocketInit {
public:
    SocketInit() {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
    }
    ~SocketInit() {
        WSACleanup();
    }
} socketInit;

class Server {
public:
    SOCKET listenSock = INVALID_SOCKET;
    SOCKET clientSock = INVALID_SOCKET;



    bool createAndBind(int port) {
        listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listenSock == INVALID_SOCKET) {
            std::cout << "socket failed: " << WSAGetLastError() << "\n";
            return false;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);

        if (bind(listenSock, (sockaddr*)&addr, sizeof(addr)) != 0) {
            std::cout << "bind failed: " << WSAGetLastError() << "\n";
            return false;
        }

        if (listen(listenSock, 1) != 0) {
            std::cout << "listen failed: " << WSAGetLastError() << "\n";
            return false;
        }

        std::cout << "Waiting for connection...\n";
        return true;
    }

    bool acceptClient() {
        clientSock = accept(listenSock, nullptr, nullptr);
        if (clientSock == INVALID_SOCKET) {
            std::cout << "accept failed: " << WSAGetLastError() << "\n";
            return false;
        }
        std::cout << "Client connected!\n";
        return true;
    }
    bool Reconnect() {
        while (true)
        {
            closeAll();
            if (!createAndBind(8080)) return false;
            if (!acceptClient()) return false;
			std::this_thread::sleep_for(std::chrono::seconds(5));
            return true;

        }
    }
    bool sendMessage(const std::string& msg) {
        uint32_t nlen = htonl((uint32_t)msg.size());
        if (send(clientSock, (char*)&nlen, sizeof(nlen), 0) <= 0) return false;
        if (!msg.empty() && send(clientSock, msg.data(), (int)msg.size(), 0) <= 0) return false;
        return true;
    }

    std::string receiveMessage() {
        uint32_t len;
        int r = recv(clientSock, (char*)&len, sizeof(len), 0);
        if (r <= 0) return "";

        len = ntohl(len);

        std::string msg;
        msg.reserve(len);

        char buffer[1024];
        int received = 0;
        while (received < (int)len) {
            int bytes = recv(clientSock, buffer, min(1024, (int)len - received), 0);
            if (bytes <= 0) return "";
            msg.append(buffer, bytes);
            received += bytes;
        }
        return msg;
    }

    void closeAll() {
        if (clientSock != INVALID_SOCKET) closesocket(clientSock);
        if (listenSock != INVALID_SOCKET) closesocket(listenSock);
    }
};
class Commands {
public:
    static void printHelp() {
        std::cout << "Available commands:\n";
        std::cout << "/help - Show this help message\n";
        std::cout << "/exit - Exit the application\n";

	}
    static void exitApplication() {
        std::cout << "Exiting application...\n";
        exit(0);
	}
    static void unknownCommand() {
		std::cout << "Unknown command. Type /help for a list of commands.\n";
};
};


int main() {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;

        Server server;
        if (!server.createAndBind(8080)) return 1;
        if (!server.acceptClient()) return 1;

        std::thread receiver([&] {
            while (true) {
                auto msg = server.receiveMessage();
                if (msg.empty()) server.Reconnect();
                std::cout << "Client: " << msg << "\n";


                if (msg[0] == '/') 
                {
                    if (msg == "/help") {
                        Commands::printHelp();
						server.sendMessage("/help - Show this help message\n/exit - Exit the application\n");

                    }
                    else if (msg == "/exit") {
                        server.sendMessage("Exit occured");
                        Commands::exitApplication();
                        

                    }
                    else {
                        Commands::unknownCommand();
                    }
                }

            }});

            std::thread sender([&] {
                std::string msg;
                while (std::getline(std::cin, msg)) {
                    server.sendMessage(msg);
                }
                });

            receiver.join();
            sender.join();

            server.closeAll();
            WSACleanup();
    }
