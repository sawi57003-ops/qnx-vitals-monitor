# Build with: source $QNX_SDP/qnxsdp-env.sh && make
SDP ?= $(HOME)/qnx/sdp-8.0
ARCH ?= x86_64
TARGET = vitals-$(ARCH)
CC = qcc
CFLAGS = -Vgcc_nto$(ARCH)

all: $(TARGET)

$(TARGET): vitals.c
	$(CC) $(CFLAGS) -o $@ vitals.c -lm

clean:
	rm -f vitals-x86_64 vitals-aarch64le

.PHONY: all clean
