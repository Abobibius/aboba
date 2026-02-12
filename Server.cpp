/*

SERVER CODE

*/
#include <iostream>
#include <thread>
#include <vector>
#include <mutex>
#include <atomic>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <string>
#include <algorithm>
#include <sstream>
#include <cctype>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#pragma comment(lib, "ws2_32.lib")

using namespace std;


class LobbyManager {
    std::unordered_map<int, std::unordered_set<SOCKET>> rooms;  // roomId -> clients
    std::unordered_map<SOCKET, int> clientRoom;                 // client -> roomId
    std::mutex mtx;

public:
    bool createRoom(int id) {
        if (id <= 0) return false;
        std::lock_guard<std::mutex> lock(mtx);
        return rooms.emplace(id, std::unordered_set<SOCKET>{}).second;
    }

    bool joinRoom(int id, SOCKET client) {
        if (id <= 0) return false;
        std::lock_guard<std::mutex> lock(mtx);

        // auto-create (якщо хочеш тільки join в існуючу — прибери ці 2 рядки)
        rooms.try_emplace(id, std::unordered_set<SOCKET>{});

        // якщо клієнт уже був у кімнаті — прибрати звідти
        auto itOld = clientRoom.find(client);
        if (itOld != clientRoom.end()) {
            int oldId = itOld->second;
            auto itR = rooms.find(oldId);
            if (itR != rooms.end()) itR->second.erase(client);
        }

        rooms[id].insert(client);
        clientRoom[client] = id;
        return true;
    }

    void leaveRoom(SOCKET client) {
        std::lock_guard<std::mutex> lock(mtx);
        auto it = clientRoom.find(client);
        if (it == clientRoom.end()) return;

        int id = it->second;
        clientRoom.erase(it);

        auto itR = rooms.find(id);
        if (itR != rooms.end()) {
            itR->second.erase(client);
            if (itR->second.empty()) rooms.erase(itR); // опційно: видаляти порожні
        }
    }

    bool getRoomOf(SOCKET client, int& outRoomId) {
        std::lock_guard<std::mutex> lock(mtx);
        auto it = clientRoom.find(client);
        if (it == clientRoom.end()) return false;
        outRoomId = it->second;
        return true;
    }

    // Повертає копію списку сокетів кімнати (щоб не тримати mutex під час send)
    std::vector<SOCKET> getRoomClientsCopy(int roomId) {
        std::lock_guard<std::mutex> lock(mtx);
        std::vector<SOCKET> res;
        auto it = rooms.find(roomId);
        if (it == rooms.end()) return res;
        res.reserve(it->second.size());
        for (auto s : it->second) res.push_back(s);
        return res;
    }

    void removeClientFully(SOCKET client) {
        leaveRoom(client);
    }
};
class myCom {
public:
    bool isCommand(const std::string& msg) {
        return !msg.empty() && msg[0] == '/';
}

    bool isDigitChar(char c) {
        return std::isdigit((unsigned char)c) != 0;
    }

    int Attribute(const std::string& command) {
        // знайти пробіл
        size_t pos = command.find(' ');
        if (pos == std::string::npos) return -1;

        // пропустити пробіли
        size_t i = pos;
        while (i < command.size() && command[i] == ' ') ++i;
        if (i >= command.size()) return -1;

        // взяти токен до наступного пробілу
        size_t j = i;
        while (j < command.size() && command[j] != ' ') ++j;

        std::string par = command.substr(i, j - i);

        // перевірка що це int (тільки цифри)
        for (char c : par) {
            if (!isDigitChar(c)) return -1;
        }

        // конвертація
        try {
            return std::stoi(par);
        }
        catch (...) {
            return -1; // переповнення або інше
        }
    }

};
class SocketInit {
public:
    SocketInit() { WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa); }
    ~SocketInit() { WSACleanup(); }
} socketInit;

class Server {
public:
    SOCKET listenSock = INVALID_SOCKET;

    std::vector<SOCKET> clients;
    std::mutex clientsMtx;
    std::atomic<bool> running{ true };

    myCom atribute;
    LobbyManager lobby;

    bool createAndBind(int port) {
        listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listenSock == INVALID_SOCKET) {
            std::cout << "socket failed: " << WSAGetLastError() << "\n";
            return false;
        }

        // (опційно) щоб легше переживати рестарти
        BOOL reuse = TRUE;
        setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, (char*)&reuse, sizeof(reuse));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);

        if (bind(listenSock, (sockaddr*)&addr, sizeof(addr)) != 0) {
            std::cout << "bind failed: " << WSAGetLastError() << "\n";
            return false;
        }

        if (listen(listenSock, SOMAXCONN) != 0) {
            std::cout << "listen failed: " << WSAGetLastError() << "\n";
            return false;
        }

        std::cout << "Listening on port " << port << "...\n";
        return true;
    }

    // Надіслати одному клієнту
    static bool sendMessage(SOCKET sock, const std::string& msg) {
        uint32_t nlen = htonl((uint32_t)msg.size());
        if (send(sock, (char*)&nlen, sizeof(nlen), 0) <= 0) return false;
        if (!msg.empty() && send(sock, msg.data(), (int)msg.size(), 0) <= 0) return false;
        return true;
    }

    // Прочитати від одного клієнта
    static bool receiveMessage(SOCKET sock, std::string& out) {
        uint32_t lenN = 0;
        int r = recv(sock, (char*)&lenN, sizeof(lenN), 0);
        if (r <= 0) return false;

        uint32_t len = ntohl(lenN);
        out.clear();
        out.reserve(len);

        char buffer[1024];
        uint32_t received = 0;
        while (received < len) {
            int chunk = (int)min<uint32_t>(1024, len - received);
            int bytes = recv(sock, buffer, chunk, 0);
            if (bytes <= 0) return false;
            out.append(buffer, bytes);
            received += (uint32_t)bytes;
        }
        return true;
    }

    void broadcast(const std::string& msg) {
        std::lock_guard<std::mutex> lock(clientsMtx);
        for (auto it = clients.begin(); it != clients.end();) {
            if (!sendMessage(*it, msg)) {
                closesocket(*it);
                it = clients.erase(it);
            }
            else {
                ++it;
            }
        }
    }

    void removeClient(SOCKET sock) {
        std::lock_guard<std::mutex> lock(clientsMtx);
        auto it = std::find(clients.begin(), clients.end(), sock);
        if (it != clients.end()) clients.erase(it);
    }

    void clientWorker(SOCKET sock) {
        std::cout << "Client connected: " << sock << "\n";
        std::string msg;
        lobby.joinRoom(0, sock);
        sendMessage(sock, "Welcome! You are in lobby 0. Use /lobby <id> to switch.");


        while (running) {
            int roomId = 0;
            if (!receiveMessage(sock, msg)) break; // клієнт відвалився
            if (msg.empty()) continue;
            if (!lobby.getRoomOf(sock, roomId)) {

                lobby.joinRoom(0, sock);
                roomId = 0;
            }
            std::cout << "Client[" << sock << "] (Lobby " << roomId << "): " << msg << "\n";
            if (atribute.isCommand(msg)) {
                size_t pos = msg.find(' ');
                std::string command = (pos == std::string::npos) ? msg : msg.substr(0, pos);

                if (command == "/help") {
                    sendMessage(sock,
                        "/help - Show this help message\n"
                        "/exit - Exit the application\n"
                        "/lobby <id> - join/create lobby\n"
                    );
                }
                else if (command == "/exit") {
                    sendMessage(sock, "Bye!");
                    break;
                }
                else if (command == "/where") {
                    int roomId = 0;
                    if (lobby.getRoomOf(sock, roomId))
                        sendMessage(sock, "You are in lobby " + std::to_string(roomId));
                    else
                        sendMessage(sock, "You are not in any lobby (bug)");
                }
                else if (command == "/lobby") {
                    int number = atribute.Attribute(msg);
                    if (number <= 0) {
                        sendMessage(sock, "Usage: /lobby <positive_id>)");
                        continue;
                    }
                    lobby.joinRoom(number, sock);
                    sendMessage(sock, "Joined lobby " + std::to_string(number));
                }
                else {
                    sendMessage(sock, "Unknown command. Type /help\n");
                }
            }
            else {
                if (!lobby.getRoomOf(sock, roomId)) {
                  
                    lobby.joinRoom(0, sock);
                    roomId = 0;
                }
                
                auto members = lobby.getRoomClientsCopy(roomId);
                for (SOCKET other : members) {
                    if (other == sock) continue;
                    sendMessage(other, "Lobby[" + std::to_string(roomId) + "]" + "Client[" + to_string(sock) + "]" + msg);
                }
            }
        }

        lobby.removeClientFully(sock);
        closesocket(sock);
        removeClient(sock);
        
    }

    void acceptLoop() {
        while (running) {
            SOCKET sock = accept(listenSock, nullptr, nullptr);
            if (sock == INVALID_SOCKET) {
                if (!running) break;
                std::cout << "accept failed: " << WSAGetLastError() << "\n";
                continue;
            }

            {
                std::lock_guard<std::mutex> lock(clientsMtx);
                clients.push_back(sock);
            }

            std::thread(&Server::clientWorker, this, sock).detach();
        }
    }

    void stop() {
        running = false;
        if (listenSock != INVALID_SOCKET) closesocket(listenSock);

        std::lock_guard<std::mutex> lock(clientsMtx);
        for (SOCKET s : clients) closesocket(s);
        clients.clear();
    }
};

int main() {
    Server server;
    if (!server.createAndBind(8080)) return 1;

    std::thread acceptThread([&] { server.acceptLoop(); });

    // Консоль сервера: шле всім клієнтам
    std::string msg;
    while (std::getline(std::cin, msg)) {
        if (msg == "/exit") break;
        server.broadcast("Server: " + msg);
    }

    server.stop();
    acceptThread.join();
    return 0;
}
