#include <arpa/inet.h>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <vector>
#include <unistd.h>
#include <sys/socket.h>

namespace proto {
constexpr uint16_t DEFAULT_PORT = 5050;
constexpr uint8_t VERSION = 1;
constexpr uint8_t HELLO = 1;
constexpr uint8_t CHALLENGE = 2;
constexpr uint8_t ANSWER = 3;
constexpr uint8_t RESULT = 4;
constexpr uint32_t MAX_N = 100000;
const char MAGIC[4] = {'S','U','M','1'};
}

static bool sendAll(int fd, const void* data, size_t len) {
    const char* p = static_cast<const char*>(data);
    while (len > 0) {
        ssize_t n = send(fd, p, len, 0);
        if (n <= 0) return false;
        p += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

static bool recvAll(int fd, void* data, size_t len) {
    char* p = static_cast<char*>(data);
    while (len > 0) {
        ssize_t n = recv(fd, p, len, 0);
        if (n <= 0) return false;
        p += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

static uint64_t htonll(uint64_t x) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return (static_cast<uint64_t>(htonl(static_cast<uint32_t>(x & 0xffffffffULL))) << 32) |
           htonl(static_cast<uint32_t>(x >> 32));
#else
    return x;
#endif
}

static uint64_t ntohll(uint64_t x) { return htonll(x); }

struct Header {
    char magic[4];
    uint8_t version;
    uint8_t type;
    uint16_t reserved;
};

static bool sendHeader(int fd, uint8_t type) {
    Header h{};
    std::memcpy(h.magic, proto::MAGIC, 4);
    h.version = proto::VERSION;
    h.type = type;
    h.reserved = 0;
    return sendAll(fd, &h, sizeof(h));
}

static bool recvHeader(int fd, uint8_t expectedType) {
    Header h{};
    if (!recvAll(fd, &h, sizeof(h))) return false;
    if (std::memcmp(h.magic, proto::MAGIC, 4) != 0 || h.version != proto::VERSION || h.type != expectedType) {
        std::cerr << "Protocol error: invalid header.\n";
        return false;
    }
    return true;
}

static std::vector<int32_t> makeNumbers(uint32_t n) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int32_t> dist(-1000, 1000);
    std::vector<int32_t> values(n);
    for (auto& x : values) x = dist(gen);
    return values;
}

int main(int argc, char* argv[]) {
    uint16_t port = proto::DEFAULT_PORT;
    uint32_t maxN = 10;

    if (argc >= 2) {
        int p = std::stoi(argv[1]);
        if (p < 1 || p > 65535) {
            std::cerr << "Invalid port.\n";
            return 1;
        }
        port = static_cast<uint16_t>(p);
    }
    if (argc >= 3) {
        unsigned long requested = std::stoul(argv[2]);
        if (requested == 0 || requested > proto::MAX_N) {
            std::cerr << "N must be between 1 and " << proto::MAX_N << ".\n";
            return 1;
        }
        maxN = static_cast<uint32_t>(requested);
    }

    int listenFd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd < 0) {
        perror("socket");
        return 1;
    }

    int opt = 1;
    setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("bind");
        close(listenFd);
        return 1;
    }
    if (listen(listenFd, 8) < 0) {
        perror("listen");
        close(listenFd);
        return 1;
    }

    std::cout << "SUM1 server listening on 0.0.0.0:" << port
              << " with random N from 1 to " << maxN << "\n";

    std::random_device randomDevice;
    std::mt19937 nGenerator(randomDevice());
    std::uniform_int_distribution<uint32_t> nDistribution(1, maxN);

    // Sequential server: accepts one client at a time. This keeps the base assignment simple.
    while (true) {
        sockaddr_in clientAddr{};
        socklen_t clientLen = sizeof(clientAddr);
        int clientFd = accept(listenFd, reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);
        if (clientFd < 0) {
            perror("accept");
            continue;
        }

        char ip[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &clientAddr.sin_addr, ip, sizeof(ip));
        std::cout << "Client connected: " << ip << ':' << ntohs(clientAddr.sin_port) << "\n";

        // 1) Receive application-level HELLO.
        if (!recvHeader(clientFd, proto::HELLO)) {
            std::cerr << "HELLO failed. Closing client.\n";
            close(clientFd);
            continue;
        }

        while (true) {
            // 2) Reply with CHALLENGE containing N random integers.
            uint32_t roundN = nDistribution(nGenerator);
            std::vector<int32_t> numbers = makeNumbers(roundN);
            int64_t correctSum = 0;
            for (int32_t x : numbers) correctSum += static_cast<int64_t>(x);

            if (!sendHeader(clientFd, proto::CHALLENGE)) break;
            uint32_t netN = htonl(roundN);
            if (!sendAll(clientFd, &netN, sizeof(netN))) break;
            bool ok = true;
            for (int32_t value : numbers) {
                uint32_t raw = htonl(static_cast<uint32_t>(value));
                if (!sendAll(clientFd, &raw, sizeof(raw))) {
                    ok = false;
                    break;
                }
            }
            if (!ok) break;

            std::cout << "Sent challenge: ";
            for (size_t i = 0; i < numbers.size(); ++i) {
                if (i) std::cout << ' ';
                std::cout << numbers[i];
            }
            std::cout << " | expected sum=" << correctSum << "\n";

            // 3) Receive client's answer.
            if (!recvHeader(clientFd, proto::ANSWER)) break;
            uint64_t netAnswer = 0;
            if (!recvAll(clientFd, &netAnswer, sizeof(netAnswer))) break;
            int64_t clientAnswer = static_cast<int64_t>(ntohll(netAnswer));
            uint8_t status = (clientAnswer == correctSum) ? 1 : 0;

            // 4) Return validation result and keep the connection open.
            if (!sendHeader(clientFd, proto::RESULT)) break;
            uint8_t padding[3] = {0, 0, 0};
            uint64_t netCorrect = htonll(static_cast<uint64_t>(correctSum));
            if (!sendAll(clientFd, &status, sizeof(status)) ||
                !sendAll(clientFd, padding, sizeof(padding)) ||
                !sendAll(clientFd, &netCorrect, sizeof(netCorrect))) break;

            std::cout << "Client answer=" << clientAnswer
                      << " => " << (status ? "CORRECT" : "WRONG") << "\n";
        }

        close(clientFd);
    }

    close(listenFd);
    return 0;
}
