CC      ?= cc
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra
LDFLAGS ?=
TARGET  := cshell

.PHONY: all clean test debug install

all: $(TARGET)

$(TARGET): cshell.c pipe_shim.o
	$(CC) $(CFLAGS) -o $@ cshell.c pipe_shim.o $(LDFLAGS)

pipe_shim.o: pipe_shim.c
	$(CC) $(CFLAGS) -c -o $@ $<

debug: CFLAGS := -O0 -g -std=c11 -Wall -Wextra -Wshadow
debug: clean $(TARGET)

test: $(TARGET)
	./test.sh

clean:
	rm -f $(TARGET) *.o

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/$(TARGET)
