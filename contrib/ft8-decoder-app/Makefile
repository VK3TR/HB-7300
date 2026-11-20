# Host self-test for the FT8 decoder core (ft8_core + vendored ft8_lib).
# The on-radio app (main.c) is built with jeskko's SDK, not this Makefile.
CC      ?= cc
LIB      = third_party/ft8_lib
CFLAGS  ?= -O2 -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wno-format -I . -I $(LIB)
LDLIBS   = -lm
SRC      = host_test.c ft8_core.c \
           $(LIB)/ft8/constants.c $(LIB)/ft8/crc.c $(LIB)/ft8/decode.c \
           $(LIB)/ft8/encode.c $(LIB)/ft8/ldpc.c $(LIB)/ft8/message.c $(LIB)/ft8/text.c \
           $(LIB)/common/monitor.c $(LIB)/fft/kiss_fft.c $(LIB)/fft/kiss_fftr.c

ft8_test: $(SRC)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)

.PHONY: test clean
test: ft8_test
	./ft8_test

clean:
	rm -f ft8_test
