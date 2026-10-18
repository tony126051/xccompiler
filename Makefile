CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Iinclude

SRCS    := src/main.c \
           src/type.c \
           src/preprocessor.c \
           src/lexer.c \
           src/parser.c \
           src/ir.c \
           src/ir_obf.c \
           src/vm.c \
           src/codegen_arm64.c \
           src/codegen_x86_64.c

OBJS    := $(SRCS:.c=.o)
TARGET  := xcc

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

src/%.o: src/%.c include/xcc.h
	$(CC) $(CFLAGS) -c $< -o $@

test: $(TARGET)
	./tests/run_tests.sh

clean:
	rm -f $(OBJS) $(TARGET)
