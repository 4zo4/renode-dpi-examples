/**
 * @file sim_main.cpp
 * @brief AXI RAM Verilated simulation main.
 *
 * This file contains the main function for the AXI RAM simulation using Verilator and Renode DPI integration.
 * It sets up the simulation environment, including the Verilated model, the Renode agent, and the communication channels.
 * Note: Clock is driven manually from C++ to synchronize Verilog logic with Renode protocol messages and ensure stable AXI handshakes.
 */
#include <verilated.h>
#include "verilated_vcd_c.h"
#include <exception>
#include <iostream>
#include <signal.h>
#include <sys/wait.h>
#include <poll.h>
#include "Vsim.h"
#include "Vsim___024root.h"
#include "Vsim_renode_axi_if.h"
#include "src/renode_log.h"
#include "src/renode_bus.h"
#include "src/renode_dpi.h"
#include "src/buses/bus.h"
#include "src/buses/axi-slave.h"
#include "src/communication/socket_channel.h"

#ifdef WINDOWS
#error "The co-simulation driver uses POSIX APIs and is only supported on Linux/WSL2."
#endif

#define TEST_PORT_MAIN 4444 // arbitrary port choices for testing
#define TEST_PORT_SENDER 5555
#define MAX_TRANSACTION_CYCLES 10 // max clock cycles to wait for a transaction to complete

#ifndef STR
#define STR2(x) #x
#define STR(x) STR2(x)
#endif

#if ENABLE_LOGS
#define LOG_POLL(socket) \
    std::cout << "[V-SIM] Poll socket " << socket << std::endl
#define LOG_HW_RESET() \
    addPrefix("V-SIM"); \
    printf("[INFO] Hardware Reset at vtime %lu\n", vtime);
#else // !LOGS
#define LOG_POLL(socket) do {} while(0)
#define LOG_HW_RESET() do {} while(0)
#endif

#define ENABLE_EXTRA_STATS 0
#if ENABLE_EXTRA_STATS
#define TRANSACTION_CYCLES_COUNT(tran_cycles) uint16_t tran_cycles = 0
#define STATS_PRINT_TRANSACTION_CYCLES(tran_cycles) << ", max transaction cycles: " << tran_cycles
#define STATS_TRANSACTION_CYCLES(n, tran_cycles) \
    if (n > tran_cycles) \
        tran_cycles = n
#else // !EXTRA_STATS
#define TRANSACTION_CYCLES_COUNT(tran_cycles) do {} while(0)
#define STATS_PRINT_TRANSACTION_CYCLES(tran_cycles)
#define STATS_TRANSACTION_CYCLES(n, tran_cycles) do {} while(0)
#endif

static Vsim* top = nullptr;
static AxiSlave* bus = nullptr;
static RenodeAgent *agent = nullptr;
static VerilatedVcdC* tfp = nullptr;
static uint8_t dummy_axi = 0;
static uint64_t vtime = 0;

static struct {
    uint16_t simulate;
    uint16_t handleReq;
    uint16_t poll;
} stats = {};

#ifdef ENABLE_TRACE
static void TFP_OPEN()
{
    Verilated::traceEverOn(true);
    tfp = new VerilatedVcdC;
    top->trace(tfp, 99);
    tfp->open(STR(WAVEFORM_NAME)".vcd");
}
#define TFP_DUMP do { tfp->dump(vtime++); tfp->flush(); } while(0)
#define TFP_CLOSE tfp->close()
#else // !TRACE
#define TFP_OPEN() do {} while(0)
#define TFP_DUMP do {} while(0)
#define TFP_CLOSE do {} while(0)
#endif
#define TFP_STEP(x) vtime += (x)

/**
 * @brief Init for Renode DPI integration
 * Refer to renode/plugins/IntegrationLibrary/src/renode_bus.h
 * saying definition has to be provided in sim_main.cpp of cosimulated peripheral
 * @return Pointer to the created RenodeAgent
 */
RenodeAgent* Init()
{
    if (!top) {
        top = new Vsim;
        agent = new RenodeAgent(false);
    }
    return agent;
}

static void Exit()
{
    delete agent;
    top->final();
    delete top;
}

/**
 * @brief Evaluates the Verilator model logic and dumps signal states to the trace file
 */
static void topEval()
{
    top->eval();
    TFP_DUMP;
}

/**
 * @brief Provides the current simulation time to the Verilator engine for trace synchronization
 * @return double Current simulation time
 */
double sc_time_stamp()
{
    return vtime;
}

/**
 * @brief Non-blocking poll to check for pending socket data without consuming CPU cycles during idle period
 * @param socket The socket file descriptor to check
 * @return bool True if there is data available to read, false otherwise
 */
bool isMessagePending(int socket)
{
    static struct pollfd pfd = { .fd = -1, .events = POLLIN };
    if (pfd.fd != socket) {
        pfd.fd = socket;
        LOG_POLL(socket);
    }

    int ret = poll(&pfd, 1, 0);

    if (ret > 0 && (pfd.revents & POLLIN)) {
        stats.poll++;
        return true;
    }

    if (ret < 0) {
        perror("[V-SIM] FATAL: Poll error");
        exit(1);
    }

    return false;
}

#ifdef ENABLE_TEST
static volatile sig_atomic_t testDriverReady = 0;
static pid_t testDriverPid = 0;

void runTestDriver(int port_main, int port_sender);
void handle_sigusr1(int sig) {
    testDriverReady = 1;
}

static void startTestDriver(int port_main, int port_sender)
{
    signal(SIGUSR1, handle_sigusr1);

    testDriverPid = fork();

    if (testDriverPid == 0) {
        runTestDriver(port_main, port_sender);
        exit(0);
    } else {
        int timeout = 0;
        while (!testDriverReady) {
            int status;
            pid_t result = waitpid(testDriverPid, &status, WNOHANG);
            if (result == -1) {
                std::cerr << "[V-SIM] FATAL: Failed to start test driver" << std::endl;
                exit(1);
            }
            if (timeout >= 1000) {
                std::cerr << "[V-SIM] FATAL: Timeout waiting for test driver signal" << std::endl;
                kill(testDriverPid, SIGKILL);
                exit(1);
            }
            usleep(1000);
            timeout++;
        }
    }
}

static bool isTestDriverExit(void) {
    int status;
    if (waitpid(testDriverPid, &status, WNOHANG) != 0) {
        addPrefix("V-SIM");
        std::cout << "Test driver exited. Terminating simulation..." << std::endl
                  << "Func stats: simulate " << stats.simulate << "/" << stats.handleReq
                  << " poll " << stats.poll STATS_PRINT_TRANSACTION_CYCLES(tran_cycles) << std::endl;
        return true;
    }
    return false;
}

#define ON_TEST_DRV_EXIT() \
    if (isTestDriverExit()) \
        break
#else // !TEST
#define ON_TEST_DRV_EXIT() do {} while(0)
#endif

/**
 * @brief Bind the AXI bus signals to the Verilated model's ports.
 */
static void bindAxiBus(AxiSlave* bus, Vsim* top)
{
    auto* axi_if = top->rootp->__PVT__sim__DOT__axi;
    auto* root   = top->rootp;

    bus->awlock = bus->arlock = &dummy_axi;
    bus->awcache = bus->arcache = &dummy_axi;
    bus->awprot = bus->arprot = &dummy_axi;
    bus->awqos = bus->arqos = &dummy_axi;
    bus->awregion = bus->arregion = &dummy_axi;

    bus->awlen   = &axi_if->awlen;
    bus->awsize  = &axi_if->awsize;
    bus->awburst = &axi_if->awburst;
    bus->arlen   = &axi_if->arlen;
    bus->arsize  = &axi_if->arsize;
    bus->arburst = &axi_if->arburst;

    bus->aclk    = &root->clk;
    bus->aresetn = &axi_if->areset_n;
    bus->awaddr  = (uint32_t*)&axi_if->awaddr;
    bus->awvalid = &axi_if->awvalid;
    bus->wdata   = (uint32_t*)&axi_if->wdata;
    bus->wstrb   = &axi_if->wstrb;
    bus->wvalid  = &axi_if->wvalid;
    bus->bready  = &axi_if->bready;
    bus->araddr  = (uint32_t*)&axi_if->araddr;
    bus->arvalid = &axi_if->arvalid;
    bus->rready  = &axi_if->rready;

    bus->awready = &root->sim__DOT__dut__DOT__s_axi_awready_reg;
    bus->wready  = &root->sim__DOT__dut__DOT__s_axi_wready_reg;
    bus->arready = &root->sim__DOT__dut__DOT__s_axi_arready_reg;
    bus->bvalid  = &axi_if->bvalid;
    bus->rvalid  = &root->sim__DOT__dut__DOT__s_axi_rvalid_reg;
    bus->rdata   = (uint32_t*)&root->sim__DOT__dut__DOT__s_axi_rdata_reg;

    bus->awid = (uint8_t*)&axi_if->awid;
    bus->arid = (uint8_t*)&axi_if->arid;
    bus->rid = (uint8_t*)&axi_if->rid;
    bus->bid = (uint8_t*)&axi_if->bid;
    bus->rlast = &axi_if->rlast;

    bus->bresp = &axi_if->bresp;
    bus->rresp = &axi_if->rresp;
    bus->wlast = &axi_if->wlast;
}

int main(int argc, char** argv)
{
    set_timestamp();

#ifdef ENABLE_TEST
    addPrefix("V-SIM");
    std::cout << "Starting cosimulated axi ram test" << std::endl;
    std::string ren_rx = STR(TEST_PORT_MAIN);
    std::string ren_tx = STR(TEST_PORT_SENDER);
    std::string addr   = "127.0.0.1";
    startTestDriver(TEST_PORT_MAIN, TEST_PORT_SENDER);
    std::cout << "[V-SIM] Test driver PID: " << testDriverPid << std::endl;
#else // connect to Renode Server
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0] << " <ReceiverPort> <SenderPort> <Address>" << std::endl;
        return 1;
    }

    std::string ren_rx = argv[1];
    std::string ren_tx = argv[2];
    std::string addr   = argv[3];
    addPrefix("V-SIM");
    std::cout << "[V-SIM] Starting cosimulated axi ram..." << std::endl;
#endif
    std::string plus_r = "+RENODE_RECEIVER_PORT=" + ren_rx;
    std::string plus_s = "+RENODE_SENDER_PORT=" + ren_tx;
    std::string plus_a = "+RENODE_ADDRESS=" + addr;

    std::vector<char*> v_argv;
    v_argv.push_back(argv[0]);
    v_argv.push_back((char*)plus_r.c_str());
    v_argv.push_back((char*)plus_s.c_str());
    v_argv.push_back((char*)plus_a.c_str());
    v_argv.push_back(nullptr);
    int v_argc = (int)v_argv.size() - 1;
    Verilated::commandArgs(v_argc, v_argv.data());

    Init();
    TFP_OPEN();
    TRANSACTION_CYCLES_COUNT(tran_cycles);
    SocketCommunicationChannel* socketChannel = nullptr;
    int mainSocket = -1;

    while (!Verilated::gotFinish()) {
        ON_TEST_DRV_EXIT();
        if (!renodeDPIIsConnected()) {
            top->rootp->clk = !top->rootp->clk;
            topEval(); TFP_STEP(50);
            if (!socketChannel) {
                usleep(100);
            } else {
                addPrefix("V-SIM");
                std::cout << "Connection closed" << std::endl << "Func stats: simulate "
                          << stats.simulate << "/" << stats.handleReq << " poll " << stats.poll
                          STATS_PRINT_TRANSACTION_CYCLES(tran_cycles) << std::endl;
                break;
            }
        } else {
            if (!bus) {
                socketChannel = renodeDPIGetSocketChannel();
                agent->syncChannel(socketChannel); // sync renode DPI socket channel to agent
                mainSocket = socketChannel->getSocketDescriptor();
                bus = new AxiSlave(32, 32);
                bindAxiBus(bus, top);
                bus->evaluateModel = topEval;
                agent->addBus((BaseTargetBus*)bus);
                agent->reset();
            }

            if (!isMessagePending(mainSocket))
                usleep(1000);

            int n = 0;
            // raise clock edge to trigger 'always @(posedge clk)' in sim.sv and process a pending transaction
            top->rootp->clk = 1; topEval(); TFP_STEP(50);
            // pump the clock here to advance the hardware state machine while the hardware is handling a bus transaction
            // internal Verilog clock generation (#) in sim.sv is disabled to prevent race conditions
            while ((*bus->arvalid || *bus->awvalid) && !(*bus->rvalid || *bus->bvalid) && n++ < MAX_TRANSACTION_CYCLES) {
                top->rootp->clk = 0; topEval(); TFP_STEP(50);
                top->rootp->clk = 1; topEval(); TFP_STEP(50);
            }
            top->rootp->clk = 0; topEval(); TFP_STEP(50); // fall clock edge to complete the clock cycle
            STATS_TRANSACTION_CYCLES(n, tran_cycles);

            if (*bus->aresetn == 0) { // keep the bus always out of reset
                *bus->aresetn = 1;
                LOG_HW_RESET();
            }

            stats.handleReq += agent->simulate(NonBus{}); // process system-level messages
            stats.simulate++;
        }
    }

    TFP_CLOSE;
    Exit();
    return 0;
}
