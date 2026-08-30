CC ?= cc
CFLAGS ?= -O2
CPPFLAGS ?=
WARNINGS = -Wall -Wextra -Wpedantic

TARGET = nq666-proxy
OBJECTS = main.o netchan.o protocol.o

.PHONY: all clean test integration-test install

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

main.o: main.c netchan.h protocol.h
netchan.o: netchan.c netchan.h
protocol.o: protocol.c protocol.h

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -std=c11 -c -o $@ $<

tests: tests.c netchan.c protocol.c netchan.h protocol.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -Werror -std=c11 \
		-o $@ tests.c netchan.c protocol.c

test: tests
	./tests

integration_test: integration_test.c netchan.h $(TARGET)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -Werror -std=c11 \
		-o $@ integration_test.c

integration-test: integration_test
	./integration_test

install: $(TARGET)
	install -D -m 0755 $(TARGET) $(DESTDIR)/usr/local/bin/$(TARGET)
	install -D -m 0644 nq666-proxy.service \
		$(DESTDIR)/usr/local/lib/systemd/system/nq666-proxy.service

clean:
	rm -f $(TARGET) $(OBJECTS) tests integration_test
