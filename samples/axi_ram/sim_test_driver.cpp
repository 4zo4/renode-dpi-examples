/**
 * @file sim_test_driver.cpp
 * @brief Test driver for AXI RAM simulation.
 */
#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <iomanip>
#include <signal.h>
#include <stdint.h>
#include <time.h>
#include <poll.h>
#include <vector>

#include "src/renode.h"

#define TEST_PEER_HANGUP 0 // use 1 | 2 for hangup testing
#define MASK_DOUBLEWORD 0xFFFFFFFFULL

void addPrefix(const std::string& tag);
const char* actionToStr(Action action);
constexpr Action noResp = static_cast<Action>(-1);

bool verify_action(Action rx, Action expected) {
    if (rx == expected)
        return true;
    std::cerr << "\n\033[1;31m[TEST-DRV] FATAL: Protocol Error\033[0m" << std::endl;
    std::cerr << "Read: " << actionToStr(rx) << " (" << (int)rx << ")" << std::endl;
    std::cerr << "Expected: " << actionToStr(expected) << " (" << (int)expected << ")" << std::endl;

    exit(1);
    return false;
}

#if ENABLE_LOGS
void dump_protocol(const std::string& prefix, const Protocol& p) {
    addPrefix(prefix);
    printf("Action: %s (%d) | Addr: 0x%lx | Value: 0x%lx | Index: %d\n",
        actionToStr(static_cast<Action>(p.actionId)), p.actionId, (unsigned long)p.addr, (unsigned long)p.value, p.peripheralIndex);
}
#else
#define dump_protocol(prefix, p) do {} while(0)
#endif

bool isAsyncMsg(Action action) {
    return action == logMessage || action == interrupt;
}

Protocol receive(int mainSocket, int senderSocket) {
    struct pollfd fds[2];
    fds[0].fd = mainSocket;
    fds[0].events = POLLIN;
    fds[1].fd = senderSocket;
    fds[1].events = POLLIN;

    while (true) {
        int ret = poll(fds, 2, -1);
        if (ret > 0) {
            for (int i = 0; i < 2; i++) {
                if (fds[i].revents & POLLIN) {
                    Protocol message;
                    if (recv(fds[i].fd, &message, sizeof(Protocol), 0) > 0) {
                        dump_protocol("RX", message);
                        if (message.actionId == logMessage) {
                            uint64_t len = message.addr;
                            if (len > 0 && len < 1024) {
                                std::vector<char> buf(len + 1, 0);
                                recv(fds[i].fd, buf.data(), len, 0);
                                std::cout << buf.data() << std::endl;
                            }
                            continue;
                        }
                        return message;
                    }
                }
            }
        } else if (ret < 0) {
            perror("[TEST-DRV] Poll error");
            exit(1);
        }
    }
}

/**
 * @brief Runs the test driver for the AXI RAM simulation
 * @param port_main The port for the Renode main communication channel
 * @param port_sender The port for the Renode sender communication channel
 */
void runTestDriver(int port_main, int port_sender) {
    auto create_listener  = [](int port) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);

        if (fd < 0) {
            perror("[TEST-DRV] socket failed");
            exit(1);
        }

        int opt = 1;
        if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
            perror("[TEST-DRV] setsockopt failed");
            exit(1);
        }
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));

        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            std::cerr << "[TEST-DRV] Bind failed on port " << port << ": " << strerror(errno) << std::endl;
            exit(1);
        }
        if (listen(fd, 4) < 0) {
            std::cerr << "[TEST-DRV] Listen failed on port " << port << ": " << strerror(errno) << std::endl;
            exit(1);
        }
        return fd;
    };

    int mainListener = create_listener(port_main);
    int senderListener = create_listener(port_sender);

    kill(getppid(), SIGUSR1);

    std::cout << "[TEST-DRV] Waiting for connections on ports " << port_main << " and " << port_sender << std::endl;
    int mainSocket = accept(mainListener, NULL, NULL);
    std::cout << "[TEST-DRV] Accepted connection on main socket " << mainSocket << std::endl;
    int senderSocket = accept(senderListener, NULL, NULL);
    std::cout << "[TEST-DRV] Accepted connection on sender socket " << senderSocket << std::endl;

    int32_t peripheralId = noPeripheralIndex;
    auto exchange = [&](Action action, Action expected = noResp, uint64_t addr = 0, uint64_t data = 0) {
        Protocol p(static_cast<int32_t>(action), addr, data, peripheralId);

        dump_protocol("TX", p);
        send(mainSocket, &p, sizeof(p), 0);

        if (expected == noResp)
            return p;

        while (true) {
            Protocol msg = {};
            msg = receive(mainSocket, senderSocket);
            if (isAsyncMsg(static_cast<Action>(msg.actionId))) {
                continue;
            }

            if (peripheralId != msg.peripheralIndex)
                peripheralId = msg.peripheralIndex;
            if (!verify_action(static_cast<Action>(msg.actionId), expected))
                exit(1);
            return msg;
        }
    };

#if (TEST_PEER_HANGUP == 1)
    std::cout << "[TEST-DRV] Hangup" << std::endl;
    close(senderSocket);
    close(mainSocket);
    sleep(10);
    if (mainSocket != -1 && senderSocket != -1)
        return;
#endif
    std::cout << "[TEST-DRV] Handshake" << std::endl;
    exchange(handshake, handshake);

    std::cout << "[TEST-DRV] Reset peripheral" << std::endl;
    exchange(resetPeripheral);

    std::cout << "[TEST-DRV] Tick clock" << std::endl;
    exchange(tickClock, tickClock, 0, 100);

    // AXI memory Write Test
    uint64_t addr = 0x4;
    uint64_t val = 0x12345678CAFEDEADULL;
    std::cout << "[TEST-DRV] WriteDoubleWord 0x" << std::hex << val << " to address 0x" << std::hex << addr << std::endl;
    exchange(writeRequestDoubleWord, ok, addr, val);

    // AXI memory Read Test
    std::cout << "[TEST-DRV] ReadDoubleWord from address 0x" << std::hex << addr << std::endl;
    Protocol resp = exchange(readRequestDoubleWord, readRequest, addr);

    if (resp.value == (val & MASK_DOUBLEWORD)) {
        std::cout << "\033[1;32m[TEST-DRV] PASS: Read 0x" << std::hex << resp.value << " matches written 0x" << std::hex << (val & MASK_DOUBLEWORD) << "\033[0m" << std::endl;
    } else {
        std::cout << "\033[1;31m[TEST-DRV] FAIL: Read 0x" << std::hex << resp.value << " != 0x" << std::hex << (val & MASK_DOUBLEWORD) << "\033[0m" << std::endl;
    }

#if (TEST_PEER_HANGUP == 2)
    std::cout << "[TEST-DRV] Hangup" << std::endl;
    close(senderSocket);
    close(mainSocket);
    sleep(10);
#else // Normal Disconnect
    std::cout << "[TEST-DRV] Disconnect" << std::endl;
    exchange(disconnect, ok);

    close(senderSocket);
    close(mainSocket);
#endif
}
