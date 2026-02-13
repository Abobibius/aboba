#include <winsock2.h>
#include <ws2tcpip.h>

#include <iostream>
#include <thread>
#include <string>
#include <chrono>
#include <mutex>
#include <atomic>
#include <unordered_map>

#pragma comment(lib, "ws2_32.lib")

using namespace std;

class TcpClient {
public:
    SOCKET serverSock = INVALID_SOCKET;

    // для reconnect
    std::string serverIp;
    uint16_t serverPort = 0;

    bool connectToServer(const std::string& ip, uint16_t port) {
        serverIp = ip;
        serverPort = port;

        closeConnection(); // закриємо старий сокет, якщо був

        serverSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (serverSock == INVALID_SOCKET) {
            std::cout << "socket failed: " << WSAGetLastError() << "\n";
            return false;
        }

        sockaddr_in server{};
        server.sin_family = AF_INET;
        server.sin_port = htons(port);
        if (inet_pton(AF_INET, ip.c_str(), &server.sin_addr) != 1) {
            std::cout << "Bad IP address\n";
            closeConnection();
            return false;
        }

        if (connect(serverSock, (sockaddr*)&server, sizeof(server)) != 0) {
            std::cout << "Connect failed: " << WSAGetLastError() << "\n";
            closeConnection();
            return false;
        }

        std::cout << "Connected!\n";
        return true;
    }

    bool reconnectLoop(int retrySeconds = 5) {
        while (true) {
            std::cout << "Reconnecting...\n";
            if (connectToServer(serverIp, serverPort)) return true;
            std::this_thread::sleep_for(std::chrono::seconds(retrySeconds));
        }
    }

    bool sendMessage(const std::string& msg) {
        if (serverSock == INVALID_SOCKET) return false;

        uint32_t len = (uint32_t)msg.size();
        uint32_t lenN = htonl(len);

        if (!sendAll((const char*)&lenN, 4)) return false;
        if (len > 0 && !sendAll(msg.data(), (int)len)) return false;

        return true;
    }

    bool receiveMessage(std::string& out) {
        out.clear();
        if (serverSock == INVALID_SOCKET) return false;

        uint32_t lenN = 0;
        if (!recvAll((char*)&lenN, 4)) return false;

        uint32_t len = ntohl(lenN);

        // захист від випадкового/шкідливого розміру
        static constexpr uint32_t MAX_MSG = 256 * 1024; // 256 KB
        if (len > MAX_MSG) return false;

        out.resize(len);
        if (len > 0 && !recvAll(&out[0], (int)len)) return false;

        return true;
    }

    void closeConnection() {
        if (serverSock != INVALID_SOCKET) {
            shutdown(serverSock, SD_BOTH);
            closesocket(serverSock);
            serverSock = INVALID_SOCKET;
        }
    }

private:
    bool recvAll(char* data, int len) {
        int got = 0;
        while (got < len) {
            int r = recv(serverSock, data + got, len - got, 0);
            if (r <= 0) return false;
            got += r;
        }
        return true;
    }

    bool sendAll(const char* data, int len) {
        int sent = 0;
        while (sent < len) {
            int r = send(serverSock, data + sent, len - sent, 0);
            if (r <= 0) return false;
            sent += r;
        }
        return true;
    }
};

int main() {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;

    TcpClient client;

    // сервер
    client.connectToServer("26.112.93.93", 8080);

    std::atomic<bool> running{ true };
    std::mutex coutMtx;

    auto safePrint = [&](const std::string& s) {
        std::lock_guard<std::mutex> lock(coutMtx);
        std::cout << s << std::flush;
        };

    // ping state
    std::atomic<uint32_t> pingCounter{ 0 };
    std::mutex pingMtx;
    std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> pingSent;

    std::thread receiver([&]() {
        std::string msg;

        while (running) {
            if (client.serverSock == INVALID_SOCKET) {
                client.reconnectLoop();
                continue;
            }

            if (!client.receiveMessage(msg)) {
                safePrint("Disconnected.\n");
                client.closeConnection();
                continue;
            }

            // /pong <id> -> RTT
            if (msg.rfind("/pong", 0) == 0) {
                uint32_t id = 0;
                size_t pos = msg.find(' ');
                if (pos != std::string::npos) {
                    try { id = (uint32_t)std::stoul(msg.substr(pos + 1)); }
                    catch (...) { id = 0; }
                }

                if (id != 0) {
                    std::chrono::steady_clock::time_point start;
                    bool found = false;

                    {
                        std::lock_guard<std::mutex> lock(pingMtx);
                        auto it = pingSent.find(id);
                        if (it != pingSent.end()) {
                            start = it->second;
                            pingSent.erase(it);
                            found = true;
                        }
                    }

                    if (found) {
                        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - start
                        ).count();

                        safePrint("Ping: " + std::to_string(ms) + " ms\n");
                        continue;
                    }
                }

                safePrint("Pong\n");
                continue;
            }

            safePrint(msg + "\n");
        }
        });

    std::thread sender([&]() {
        std::string line;

        while (running && std::getline(std::cin, line)) {
            if (line == "/quit") {
                running = false;
                client.closeConnection();
                break;
            }

            if (line == "/clear") {
                system("cls");
                continue;
            }

            if (line == "/reconnect") {
                client.closeConnection();
                client.reconnectLoop();
                continue;
            }

            if (line == "/ping") {
                uint32_t id = ++pingCounter;
                {
                    std::lock_guard<std::mutex> lock(pingMtx);
                    pingSent[id] = std::chrono::steady_clock::now();
                }

                // timeout 3 сек
                std::thread([&, id]() {
                    std::this_thread::sleep_for(std::chrono::seconds(3));
                    bool stillThere = false;
                    {
                        std::lock_guard<std::mutex> lock(pingMtx);
                        stillThere = pingSent.erase(id) > 0;
                    }
                    if (stillThere) safePrint("Ping timeout\n");
                    }).detach();

                if (!client.sendMessage("/ping " + std::to_string(id))) {
                    safePrint("Send failed. Try /reconnect\n");
                }
                continue;
            }

            if (client.serverSock == INVALID_SOCKET) {
                safePrint("Not connected. Try /reconnect\n");
                continue;
            }

            if (!client.sendMessage(line)) {
                safePrint("Send failed. Reconnecting...\n");
                client.closeConnection();
            }
        }
        });

    sender.join();
    running = false;
    client.closeConnection();
    receiver.join();

    WSACleanup();
    return 0;
}
