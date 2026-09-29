#include <arpa/inet.h>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
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

int main(int argc, char* argv[]) {
    if (argc < 2 || argc > 3) {
        std::cerr << "Usage: " << argv[0] << " <server-ip> [port]\n";
        return 1;
    }

    const std::string serverIp = argv[1];
    uint16_t port = proto::DEFAULT_PORT;
    if (argc == 3) {
        int p = std::stoi(argv[2]);
        if (p < 1 || p > 65535) {
            std::cerr << "Invalid port.\n";
            return 1;
        }
        port = static_cast<uint16_t>(p);
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, serverIp.c_str(), &addr.sin_addr) != 1) {
        std::cerr << "Invalid IPv4 address.\n";
        close(sock);
        return 1;
    }

    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("connect");
        close(sock);
        return 1;
    }

    // 1) Application-level greeting, separate from the TCP SYN handshake.
    if (!sendHeader(sock, proto::HELLO)) {
        std::cerr << "Failed to send HELLO.\n";
        close(sock);
        return 1;
    }
    std::cout << "Sent HELLO to server.\n";

    // 2) Server greeting + SUM challenge: N (4 bytes) followed by N signed 32-bit integers.
    if (!recvHeader(sock, proto::CHALLENGE)) {
        close(sock);
        return 1;
    }

    uint32_t netN = 0;
    if (!recvAll(sock, &netN, sizeof(netN))) {
        std::cerr << "Failed to receive N.\n";
        close(sock);
        return 1;
    }
    uint32_t n = ntohl(netN);
    if (n == 0 || n > proto::MAX_N) {
        std::cerr << "Protocol error: invalid N = " << n << ".\n";
        close(sock);
        return 1;
    }

    std::vector<int32_t> numbers;
    numbers.reserve(n);
    int64_t sum = 0;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t raw = 0;
        if (!recvAll(sock, &raw, sizeof(raw))) {
            std::cerr << "Failed to receive number " << i << ".\n";
            close(sock);
            return 1;
        }
        int32_t value = static_cast<int32_t>(ntohl(raw));
        numbers.push_back(value);
        sum += static_cast<int64_t>(value);
    }

    std::cout << "Received " << n << " numbers: ";
    for (size_t i = 0; i < numbers.size(); ++i) {
        if (i) std::cout << ' ';
        std::cout << numbers[i];
    }
    std::cout << "\nCalculated sum = " << sum << "\n";

    // 3) Client sends its computed answer.
    if (!sendHeader(sock, proto::ANSWER)) {
        std::cerr << "Failed to send ANSWER header.\n";
        close(sock);
        return 1;
    }
    uint64_t netSum = htonll(static_cast<uint64_t>(sum));
    if (!sendAll(sock, &netSum, sizeof(netSum))) {
        std::cerr << "Failed to send sum.\n";
        close(sock);
        return 1;
    }

    // 4) Server validates and returns CORRECT/WRONG plus the correct sum.
    if (!recvHeader(sock, proto::RESULT)) {
        close(sock);
        return 1;
    }
    uint8_t status = 0;
    uint8_t padding[3]{};
    uint64_t netCorrect = 0;
    if (!recvAll(sock, &status, sizeof(status)) ||
        !recvAll(sock, padding, sizeof(padding)) ||
        !recvAll(sock, &netCorrect, sizeof(netCorrect))) {
        std::cerr << "Failed to receive RESULT payload.\n";
        close(sock);
        return 1;
    }
    int64_t correctSum = static_cast<int64_t>(ntohll(netCorrect));

    if (status == 1) {
        std::cout << "Server result: CORRECT\n";
    } else {
        std::cout << "Server result: WRONG. Correct sum = " << correctSum << "\n";
    }

    close(sock);
    return 0;
}
