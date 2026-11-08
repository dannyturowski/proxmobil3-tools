# proxmobil3-tools: Linux tools for the PROXmobil3 and the optional ESP32 accessory host side.
#
#   make              native build of every tool into build/native/
#   make arm          static ARMv7 hard-float build for the PM3 into build/armhf/
#   make test         C unit tests + Python end-to-end tests (no hardware)
#   make clean
#
# Needs only a C11 compiler and Python 3 (standard library) for the tests.  The ARM
# build needs a cross compiler, e.g. Debian/Ubuntu gcc-arm-linux-gnueabihf.

CC      ?= cc
CROSS   ?= arm-linux-gnueabihf-
PYTHON  ?= python3
CFLAGS  ?= -O2
WARN     = -std=c11 -Wall -Wextra -Wshadow -Wformat=2
ifeq ($(WERROR),1)
WARN    += -Werror
endif
ARMFLAGS = -static -march=armv7-a -mtune=cortex-a9 -mfpu=vfpv3-d16 -mfloat-abi=hard

PM3_INC  = -Ipm3/include
PM3_LIB  = pm3/src/pm3_ipp.c pm3/src/pm3_nfc.c pm3/src/pm3_serial.c pm3/src/pm3_pic32.c pm3/src/pm3_scanner.c
PM3_HDR  = $(wildcard pm3/include/*.h)
PM3_TOOLS = pm3-nfc-presence pm3-pic32-read pm3-scanner-light

ACC_INC  = -Iaccessory/shared/nori_link/src -Iaccessory/shared/hostio
ACC_LIB  = accessory/shared/nori_link/src/nori_link.c accessory/shared/nori_link/src/nori_host.c \
           accessory/shared/hostio/nori_hostio.c
ACC_HDR  = $(wildcard accessory/shared/nori_link/src/*.h accessory/shared/hostio/*.h)
ACC_TOOLS = wt32-monitor

NATIVE = $(addprefix build/native/,$(PM3_TOOLS) $(ACC_TOOLS))
ARM    = $(addprefix build/armhf/,$(PM3_TOOLS) $(ACC_TOOLS))

.PHONY: all arm test unit e2e clean
all: $(NATIVE)
arm: $(ARM)

build/native build/armhf build/test:
	mkdir -p $@

build/native/pm3-%: pm3/tools/pm3-%.c $(PM3_LIB) $(PM3_HDR) | build/native
	$(CC) $(CFLAGS) $(WARN) $(PM3_INC) -o $@ $< $(PM3_LIB)

build/armhf/pm3-%: pm3/tools/pm3-%.c $(PM3_LIB) $(PM3_HDR) | build/armhf
	$(CROSS)gcc $(CFLAGS) $(WARN) $(ARMFLAGS) $(PM3_INC) -o $@ $< $(PM3_LIB)

build/native/wt32-monitor: accessory/host/wt32-monitor.c $(ACC_LIB) $(ACC_HDR) | build/native
	$(CC) $(CFLAGS) $(WARN) $(ACC_INC) -o $@ $< $(ACC_LIB)

build/armhf/wt32-monitor: accessory/host/wt32-monitor.c $(ACC_LIB) $(ACC_HDR) | build/armhf
	$(CROSS)gcc $(CFLAGS) $(WARN) $(ARMFLAGS) $(ACC_INC) -o $@ $< $(ACC_LIB)

build/test/test_pm3: tests/unit/test_pm3.c $(PM3_LIB) $(PM3_HDR) | build/test
	$(CC) -O1 -g $(WARN) -fsanitize=address,undefined $(PM3_INC) -o $@ $< $(PM3_LIB)

unit: build/test/test_pm3
	build/test/test_pm3

e2e: all
	PM3_TOOLS_BIN=$(CURDIR)/build/native $(PYTHON) -m unittest discover -s tests/e2e -v

test: unit e2e

clean:
	rm -rf build
