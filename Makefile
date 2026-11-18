CC ?= cc
AR ?= ar
CFLAGS ?= -O2 -g
CPPFLAGS ?= -Iinclude
WARNINGS ?= -std=c99 -Wall -Wextra -Werror -pedantic -D_POSIX_C_SOURCE=200809L
LDFLAGS ?=
LDLIBS ?= -pthread
PREFIX ?= /usr/local

BUILD := build
LIB := $(BUILD)/libnextui_app.a
SRC := src/nextui_app.c src/nextui_json.c src/nextui_transport.c
OBJ := $(SRC:src/%.c=$(BUILD)/%.o)
TEST_SRC := tests/test_json.c tests/test_client.c
TEST_BIN := $(BUILD)/test_json $(BUILD)/test_client
FAKE := $(BUILD)/fake_devserv
EXAMPLE := $(BUILD)/nextui-overlay-demo

.PHONY: all lib test example clean install

all: lib

lib: $(LIB)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(WARNINGS) $(CFLAGS) -c $< -o $@

$(LIB): $(OBJ)
	$(AR) rcs $@ $(OBJ)

$(BUILD)/fake_devserv: tests/fake_devserv.c | $(BUILD)
	$(CC) $(WARNINGS) $(CFLAGS) $(LDFLAGS) $< -o $@

$(BUILD)/test_json: tests/test_json.c $(BUILD)/nextui_json.o | $(BUILD)
	$(CC) $(CPPFLAGS) $(WARNINGS) $(CFLAGS) $(LDFLAGS) $^ -o $@

$(BUILD)/test_client: tests/test_client.c $(LIB) | $(BUILD)
	$(CC) $(CPPFLAGS) $(WARNINGS) $(CFLAGS) $(LDFLAGS) $< $(LIB) $(LDLIBS) -o $@

$(EXAMPLE): examples/overlay_demo.c $(LIB) | $(BUILD)
	$(CC) $(CPPFLAGS) $(WARNINGS) $(CFLAGS) $(LDFLAGS) $< $(LIB) $(LDLIBS) -o $@

example: $(EXAMPLE)

test: $(LIB) $(BUILD)/test_json $(BUILD)/test_client $(FAKE)
	NEXTUI_FAKE_DEVSERV="$(abspath $(FAKE))" \
	NEXTUI_TEST_TMP="$(abspath $(BUILD))/tmp" \
	tests/run_tests.sh "$(abspath $(BUILD)/test_json)" "$(abspath $(BUILD)/test_client)"

install: $(LIB)
	install -d "$(DESTDIR)$(PREFIX)/include" "$(DESTDIR)$(PREFIX)/lib"
	install -m 0644 include/nextui_app.h "$(DESTDIR)$(PREFIX)/include/"
	install -m 0644 $(LIB) "$(DESTDIR)$(PREFIX)/lib/"

clean:
	rm -rf $(BUILD)
