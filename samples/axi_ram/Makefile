# Makefile for AXI RAM simulation using Verilator and Renode DPI integration.
# This Makefile compiles the SystemVerilog sources along with the Renode C++ DPI code to create a Verilated simulation executable.

MKFILE_PATH := $(abspath $(lastword $(MAKEFILE_LIST)))
MKFILE_DIR  := $(dir $(MKFILE_PATH))
RENODE_PATH = $(abspath $(MKFILE_DIR)/../../../renode/src/Plugins/CoSimulationPlugin/IntegrationLibrary)

BUILD_DIR = build

VERILATOR = verilator
NUM_THREADS = 4
TOP_MODULE = sim
WAVEFORM_NAME = sim
ENABLE_LOGS = 0
ENABLE_FILTER = 1
ENABLE_WAIT_LIMIT = 0

# --- HDL Sources ---
HDL_SOURCES = \
	$(RENODE_PATH)/hdl/renode_pkg.sv \
	$(RENODE_PATH)/hdl/imports/renode_axi_pkg.sv \
	$(RENODE_PATH)/hdl/modules/renode_inputs.sv \
	$(RENODE_PATH)/hdl/modules/renode.sv \
	$(RENODE_PATH)/hdl/modules/axi/renode_axi_if.sv \
	$(RENODE_PATH)/hdl/modules/axi/renode_axi_manager.sv \
	sim.sv \
	axi_ram.v

# --- C++ Sources ---
CPP_SOURCES = \
	$(RENODE_PATH)/src/renode_log.cpp \
	$(RENODE_PATH)/src/renode_bus.cpp \
	$(RENODE_PATH)/src/renode_dpi.cpp \
	$(RENODE_PATH)/src/buses/bus.cpp \
	$(RENODE_PATH)/src/buses/axi.cpp \
	$(RENODE_PATH)/src/buses/axi-slave.cpp \
	$(RENODE_PATH)/src/communication/socket_channel.cpp \
	$(RENODE_PATH)/libs/socket-cpp/Socket/TCPClient.cpp \
	$(RENODE_PATH)/libs/socket-cpp/Socket/Socket.cpp

MAIN_CPP = \
	sim_main.cpp

TEST_CPP_SOURCES = \
	$(MAIN_CPP) \
	sim_test_driver.cpp \

INC_FLAGS = \
	-I$(RENODE_PATH)/hdl/includes \
	-I$(RENODE_PATH) \
	-I$(RENODE_PATH)/src \
	-I$(RENODE_PATH)/libs/socket-cpp

VFLAGS = -Wno-fatal -DENABLE_LOGS=$(ENABLE_LOGS)
CFLAGS = -fPIC -g -O3 -fcoroutines $(INC_FLAGS) -DENABLE_LOGS=$(ENABLE_LOGS) -DENABLE_FILTER=$(ENABLE_FILTER) -DENABLE_WAIT_LIMIT=$(ENABLE_WAIT_LIMIT)
LDFLAGS = -lrt -lpthread

all: verilated

verilated: $(HDL_SOURCES) $(CPP_SOURCES)
	$(VERILATOR) -cc --exe -j 24 \
		--top-module $(TOP_MODULE) \
		$(HDL_SOURCES) \
		$(MAIN_CPP) \
		$(CPP_SOURCES) \
		$(INC_FLAGS) \
		-y $(RENODE_PATH)/hdl/modules/axi \
		-y $(RENODE_PATH)/hdl/imports \
		--timing \
		--threads $(NUM_THREADS) \
		--Mdir $(BUILD_DIR) \
		$(VFLAGS) \
		-CFLAGS "$(CFLAGS)" \
		-LDFLAGS "$(LDFLAGS)" \
		-o verilated \
		--build

trace: clean
	$(VERILATOR) -cc --exe -j 24 \
		--trace \
		--top-module $(TOP_MODULE) \
		$(HDL_SOURCES) \
		$(MAIN_CPP) \
		$(CPP_SOURCES) \
		$(INC_FLAGS) \
		-y $(RENODE_PATH)/hdl/modules/axi \
		-y $(RENODE_PATH)/hdl/imports \
		--timing \
		--threads $(NUM_THREADS) \
		--Mdir $(BUILD_DIR) \
		$(VFLAGS) \
		-CFLAGS "$(CFLAGS) -DENABLE_TRACE=1 -DWAVEFORM_NAME=$(WAVEFORM_NAME)" \
		-LDFLAGS "$(LDFLAGS)" \
		-o verilated \
		--build

test: clean
	lsof -t -i:4444 -i:5555 | xargs -r kill -9 2>/dev/null || true
	$(VERILATOR) -cc --exe -j 24 \
		--trace \
		--top-module $(TOP_MODULE) \
		$(HDL_SOURCES) \
		$(TEST_CPP_SOURCES) \
		$(CPP_SOURCES) \
		$(INC_FLAGS) \
		-y $(RENODE_PATH)/hdl/modules/axi \
		-y $(RENODE_PATH)/hdl/imports \
		--timing \
		--threads $(NUM_THREADS) \
		--Mdir $(BUILD_DIR) \
		$(VFLAGS) \
		-CFLAGS "$(CFLAGS) -DENABLE_TEST=1 -DENABLE_TRACE=1 -DWAVEFORM_NAME=$(WAVEFORM_NAME)" \
		-LDFLAGS "$(LDFLAGS)" \
		-o verilated \
		--build
	@echo "- Running"
	./$(BUILD_DIR)/verilated

wave:
	gtkwave sim_waveform.gtkw &

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all trace clean wave
