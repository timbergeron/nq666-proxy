CC ?= cc
ANALYZER_CC ?= gcc
CFLAGS ?= -O2
CPPFLAGS ?=
WARNINGS = -Wall -Wextra -Wpedantic -Wformat=2 -Wshadow -Wstrict-prototypes \
	-Wundef -Wwrite-strings -Wconversion -Wsign-conversion
SRC_DIR = src
TEST_DIR = tests
PACKAGING_DIR = packaging/systemd
INCLUDES = -I$(SRC_DIR)

EXEEXT =
ifeq ($(OS),Windows_NT)
EXEEXT = .exe
LDLIBS += -lws2_32
endif

TARGET = nq666-proxy$(EXEEXT)
TEST_TARGET = unit_tests$(EXEEXT)
INTEGRATION_TARGET = integration_test$(EXEEXT)
CORE_SOURCES = $(SRC_DIR)/netchan.c $(SRC_DIR)/protocol.c
HEADERS = $(SRC_DIR)/netchan.h $(SRC_DIR)/protocol.h
UNIT_TEST_SOURCE = $(TEST_DIR)/unit.c
INTEGRATION_TEST_SOURCE = $(TEST_DIR)/integration.c
OBJECTS = main.o netchan.o protocol.o
SANITIZER_FLAGS = -fsanitize=address,undefined -fno-omit-frame-pointer

.PHONY: all clean test integration-test check sanitize analyze install

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

main.o: $(SRC_DIR)/main.c $(HEADERS) $(SRC_DIR)/socket_compat.h
netchan.o: $(SRC_DIR)/netchan.c $(SRC_DIR)/netchan.h
protocol.o: $(SRC_DIR)/protocol.c $(SRC_DIR)/protocol.h

%.o: $(SRC_DIR)/%.c
	$(CC) $(CPPFLAGS) $(INCLUDES) $(CFLAGS) $(WARNINGS) -std=c11 -c -o $@ $<

$(TEST_TARGET): $(UNIT_TEST_SOURCE) $(CORE_SOURCES) $(HEADERS)
	$(CC) $(CPPFLAGS) $(INCLUDES) $(CFLAGS) $(WARNINGS) -Werror -std=c11 \
		-o $@ $(UNIT_TEST_SOURCE) $(CORE_SOURCES) $(LDLIBS)

test: $(TEST_TARGET)
	./$(TEST_TARGET)

$(INTEGRATION_TARGET): $(INTEGRATION_TEST_SOURCE) $(SRC_DIR)/netchan.h \
		$(SRC_DIR)/socket_compat.h $(TARGET)
	$(CC) $(CPPFLAGS) $(INCLUDES) $(CFLAGS) $(WARNINGS) -Werror -std=c11 \
		-o $@ $(INTEGRATION_TEST_SOURCE) $(LDLIBS)

integration-test: $(INTEGRATION_TARGET)
	./$(INTEGRATION_TARGET)

check: test integration-test

tests-sanitize: $(UNIT_TEST_SOURCE) $(CORE_SOURCES) $(HEADERS)
	$(CC) $(CPPFLAGS) $(INCLUDES) -O1 -g $(WARNINGS) -Werror -std=c11 \
		$(SANITIZER_FLAGS) \
		-o $@ $(UNIT_TEST_SOURCE) $(CORE_SOURCES)

nq666-proxy-sanitize: $(SRC_DIR)/main.c $(CORE_SOURCES) $(HEADERS) \
		$(SRC_DIR)/socket_compat.h
	$(CC) $(CPPFLAGS) $(INCLUDES) -O1 -g $(WARNINGS) -Werror -std=c11 \
		$(SANITIZER_FLAGS) \
		-o $@ $(SRC_DIR)/main.c $(CORE_SOURCES)

sanitize: tests-sanitize nq666-proxy-sanitize integration_test
	ASAN_OPTIONS="$${ASAN_OPTIONS:-detect_leaks=1:strict_string_checks=1}" \
		./tests-sanitize
	ASAN_OPTIONS="$${ASAN_OPTIONS:-detect_leaks=1:strict_string_checks=1}" \
		NQ666_PROXY=./nq666-proxy-sanitize ./integration_test

analyze:
	$(ANALYZER_CC) $(CPPFLAGS) $(INCLUDES) -O0 $(WARNINGS) -Werror -std=c11 \
		-fanalyzer -fsyntax-only $(SRC_DIR)/main.c $(CORE_SOURCES)

install: $(TARGET)
	install -D -m 0755 $(TARGET) $(DESTDIR)/usr/local/bin/$(TARGET)
	install -D -m 0644 $(PACKAGING_DIR)/nq666-proxy.service \
		$(DESTDIR)/usr/local/lib/systemd/system/nq666-proxy.service
	install -D -m 0644 $(PACKAGING_DIR)/nq666-proxy.default \
		$(DESTDIR)/usr/local/share/doc/nq666-proxy/nq666-proxy.default

clean:
	rm -f nq666-proxy nq666-proxy.exe $(OBJECTS) unit_tests unit_tests.exe \
		integration_test integration_test.exe tests-sanitize \
		nq666-proxy-sanitize
