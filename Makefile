CC ?= cc
ANALYZER_CC ?= gcc
CFLAGS ?= -O2
CPPFLAGS ?=
WARNINGS = -Wall -Wextra -Wpedantic -Wformat=2 -Wshadow -Wstrict-prototypes \
	-Wundef -Wwrite-strings -Wconversion -Wsign-conversion

EXEEXT =
ifeq ($(OS),Windows_NT)
EXEEXT = .exe
LDLIBS += -lws2_32
endif

TARGET = nq666-proxy$(EXEEXT)
TEST_TARGET = tests$(EXEEXT)
INTEGRATION_TARGET = integration_test$(EXEEXT)
OBJECTS = main.o netchan.o protocol.o
SANITIZER_FLAGS = -fsanitize=address,undefined -fno-omit-frame-pointer

.PHONY: all clean test integration-test check sanitize analyze install

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

main.o: main.c netchan.h protocol.h socket_compat.h
netchan.o: netchan.c netchan.h
protocol.o: protocol.c protocol.h

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -std=c11 -c -o $@ $<

$(TEST_TARGET): tests.c netchan.c protocol.c netchan.h protocol.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -Werror -std=c11 \
		-o $@ tests.c netchan.c protocol.c $(LDLIBS)

test: $(TEST_TARGET)
	./$(TEST_TARGET)

$(INTEGRATION_TARGET): integration_test.c netchan.h socket_compat.h $(TARGET)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -Werror -std=c11 \
		-o $@ integration_test.c $(LDLIBS)

integration-test: $(INTEGRATION_TARGET)
	./$(INTEGRATION_TARGET)

check: test integration-test

tests-sanitize: tests.c netchan.c protocol.c netchan.h protocol.h
	$(CC) $(CPPFLAGS) -O1 -g $(WARNINGS) -Werror -std=c11 \
		$(SANITIZER_FLAGS) \
		-o $@ tests.c netchan.c protocol.c

nq666-proxy-sanitize: main.c netchan.c protocol.c netchan.h protocol.h \
		socket_compat.h
	$(CC) $(CPPFLAGS) -O1 -g $(WARNINGS) -Werror -std=c11 \
		$(SANITIZER_FLAGS) \
		-o $@ main.c netchan.c protocol.c

sanitize: tests-sanitize nq666-proxy-sanitize integration_test
	ASAN_OPTIONS="$${ASAN_OPTIONS:-detect_leaks=1:strict_string_checks=1}" \
		./tests-sanitize
	ASAN_OPTIONS="$${ASAN_OPTIONS:-detect_leaks=1:strict_string_checks=1}" \
		NQ666_PROXY=./nq666-proxy-sanitize ./integration_test

analyze:
	$(ANALYZER_CC) $(CPPFLAGS) -O0 $(WARNINGS) -Werror -std=c11 -fanalyzer \
		-fsyntax-only main.c netchan.c protocol.c

install: $(TARGET)
	install -D -m 0755 $(TARGET) $(DESTDIR)/usr/local/bin/$(TARGET)
	install -D -m 0644 nq666-proxy.service \
		$(DESTDIR)/usr/local/lib/systemd/system/nq666-proxy.service
	install -D -m 0644 nq666-proxy.default \
		$(DESTDIR)/usr/local/share/doc/nq666-proxy/nq666-proxy.default

clean:
	rm -f nq666-proxy nq666-proxy.exe $(OBJECTS) tests tests.exe \
		integration_test integration_test.exe tests-sanitize \
		nq666-proxy-sanitize
