# VitalGuard — host (Linux) build.
#
#   make            build all processes + unit tests into bin/ and tests/
#   make test       build and run the whole test suite
#   make clean
#
# The QNX target build (qcc cross-compile + mkqnximage) will live in
# qemu/ once the SDP is installed; see qemu/README.md.

CC      = gcc
CFLAGS  = -std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
          -Wall -Wextra -O2 -g
INCLUDES = -Isrc/common -Isrc/signals -Isrc/dsp -Isrc/alarm \
           -Isrc/ipc -Isrc/supervisor
LDLIBS  = -lm

BUILD = build
BIN   = bin

CORE_SRC = src/common/vg_time.c src/common/vg_log.c src/common/vg_hb.c \
           src/signals/ecg.c src/signals/spo2.c src/signals/resp.c \
           src/dsp/dsp.c \
           src/alarm/alarm.c \
           src/ipc/ipc_posix.c src/ipc/ipc_qnx.c \
           src/supervisor/supervisor.c
CORE_OBJ = $(CORE_SRC:%.c=$(BUILD)/%.o)
LIB      = $(BUILD)/libvg.a
HEADERS  = $(wildcard src/*/*.h)

APPS = $(BIN)/vitalgen $(BIN)/vitaldsp $(BIN)/alarmgr \
       $(BIN)/vgsupervisor $(BIN)/faultctl $(BIN)/vghog
TESTS = tests/test_dsp tests/test_alarm tests/test_ipc

all: $(APPS) $(TESTS)

$(BUILD)/%.o: %.c $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(LIB): $(CORE_OBJ)
	@mkdir -p $(BUILD)
	ar rcs $@ $(CORE_OBJ)

$(BIN)/vitalgen: src/apps/vitalgen.c $(LIB)
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(LIB) -o $@ $(LDLIBS)

$(BIN)/vitaldsp: src/apps/vitaldsp.c $(LIB)
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(LIB) -o $@ $(LDLIBS)

$(BIN)/alarmgr: src/apps/alarmgr.c $(LIB)
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(LIB) -o $@ $(LDLIBS)

$(BIN)/vgsupervisor: src/apps/vgsupervisor.c $(LIB)
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(LIB) -o $@ $(LDLIBS)

$(BIN)/faultctl: src/faults/faultctl.c $(LIB)
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(LIB) -o $@ $(LDLIBS)

$(BIN)/vghog: src/faults/vghog.c $(LIB)
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(LIB) -o $@ $(LDLIBS)

tests/test_dsp: tests/test_dsp.c $(LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(LIB) -o $@ $(LDLIBS)

tests/test_alarm: tests/test_alarm.c $(LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(LIB) -o $@ $(LDLIBS)

tests/test_ipc: tests/test_ipc.c $(LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(LIB) -o $@ $(LDLIBS)

test: all
	bash tests/run_tests.sh

clean:
	rm -rf $(BUILD) $(BIN) $(TESTS)

.PHONY: all test clean
