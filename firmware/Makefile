# ECU-25 firmware — host (SIL) build.
# Targets:  make test   — build + run unit/scenario tests
#           make demo   — build + run the narrated AMF cycle demo
CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O2 -g

CORE := core/gcu_config.c core/engine_fsm.c core/protection.c \
        core/amf_fsm.c core/gcu_app.c core/j1939.c
SIM := sim/plant.c

build:
	mkdir -p build

build/test_all: $(CORE) $(SIM) tests/test_all.c | build
	$(CC) $(CFLAGS) $(CORE) $(SIM) tests/test_all.c -o $@

build/sim: $(CORE) $(SIM) sim/main_sim.c | build
	$(CC) $(CFLAGS) $(CORE) $(SIM) sim/main_sim.c -o $@

test: build/test_all
	./build/test_all

demo: build/sim
	./build/sim

clean:
	rm -rf build

.PHONY: build test demo clean
