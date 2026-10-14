CC	= gcc
CFLAGS	= -Wall -Wextra -g -O0

# ---- layout ---------------------------------------------------------------
# Each component builds into its own bin/: objects, deps and the final
# artifact stay next to the code they come from.
LIB_DIR		= lib/common
CLIENT_DIR	= client
SERVER_DIR	= server
LOADGEN_DIR	= tools/loadgen

CPPFLAGS	+= -I$(LIB_DIR)/include -I$(CLIENT_DIR)/include -I$(SERVER_DIR)/include
LDFLAGS		+=
LDLIBS		+=

# The server authenticates devices with HMAC-SHA256 from OpenSSL (openssl-devel).
# Only the server links it; the client does not need it.
SERVER_LDLIBS	= -lcrypto
# The load generator does the client side of the handshake and session crypto with the
# same code the server uses (lib/common secure.c + the server's OpenSSL crypt.c), and runs
# a sender and a receiver thread.
LOADGEN_LDLIBS	= -lcrypto -lpthread
LOADGEN_CRYPT	= $(SERVER_DIR)/bin/crypt.o

# ---- sources / objects ----------------------------------------------------
LIB_SRCS	= $(wildcard $(LIB_DIR)/src/*.c)
CLIENT_SRCS	= $(wildcard $(CLIENT_DIR)/src/*.c)
SERVER_SRCS	= $(wildcard $(SERVER_DIR)/src/*.c)
LOADGEN_SRCS	= $(wildcard $(LOADGEN_DIR)/*.c)

LIB_OBJS	= $(patsubst $(LIB_DIR)/src/%.c,$(LIB_DIR)/bin/%.o,$(LIB_SRCS))
CLIENT_OBJS	= $(patsubst $(CLIENT_DIR)/src/%.c,$(CLIENT_DIR)/bin/%.o,$(CLIENT_SRCS))
SERVER_OBJS	= $(patsubst $(SERVER_DIR)/src/%.c,$(SERVER_DIR)/bin/%.o,$(SERVER_SRCS))
LOADGEN_OBJS	= $(patsubst $(LOADGEN_DIR)/%.c,$(LOADGEN_DIR)/bin/%.o,$(LOADGEN_SRCS))

OBJS		= $(LIB_OBJS) $(CLIENT_OBJS) $(SERVER_OBJS) $(LOADGEN_OBJS)
DEPS		= $(OBJS:.o=.d)

# ---- artifacts ------------------------------------------------------------
LIB		= $(LIB_DIR)/bin/libcommon.a
CLIENT_BIN	= $(CLIENT_DIR)/bin/vpn_client
SERVER_BIN	= $(SERVER_DIR)/bin/vpn_server
LOADGEN_BIN	= $(LOADGEN_DIR)/bin/loadgen

# ---- top level ------------------------------------------------------------
.PHONY: all build lib client server tools clean distclean test help
.DEFAULT_GOAL := all

all: lib client server tools
build: all

lib: $(LIB)

# ---- static library -------------------------------------------------------
$(LIB): $(LIB_OBJS)
	@mkdir -p $(@D)
	$(AR) rcs $@ $^

# ---- executables ----------------------------------------------------------
# client/src and server/src may still be empty; skip instead of failing to link.
ifeq ($(strip $(CLIENT_SRCS)),)
client:
	@echo "skip client: no sources in $(CLIENT_DIR)/src"
else
client: $(CLIENT_BIN)
endif

ifeq ($(strip $(SERVER_SRCS)),)
server:
	@echo "skip server: no sources in $(SERVER_DIR)/src"
else
server: $(SERVER_BIN)
endif

tools: $(LOADGEN_BIN)

$(LOADGEN_BIN): $(LOADGEN_OBJS) $(LOADGEN_CRYPT) $(LIB)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(LOADGEN_OBJS) $(LOADGEN_CRYPT) $(LIB) $(LOADGEN_LDLIBS)

$(CLIENT_BIN): $(CLIENT_OBJS) $(LIB)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(CLIENT_OBJS) $(LIB) $(LDLIBS)

$(SERVER_BIN): $(SERVER_OBJS) $(LIB)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SERVER_OBJS) $(LIB) $(LDLIBS) $(SERVER_LDLIBS)

# ---- compile --------------------------------------------------------------
# <component>/src/foo.c -> <component>/bin/foo.o (+ foo.d for header tracking)
$(LIB_DIR)/bin/%.o: $(LIB_DIR)/src/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(CPPFLAGS) -MMD -MP -c $< -o $@

$(CLIENT_DIR)/bin/%.o: $(CLIENT_DIR)/src/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(CPPFLAGS) -MMD -MP -c $< -o $@

$(SERVER_DIR)/bin/%.o: $(SERVER_DIR)/src/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(CPPFLAGS) -MMD -MP -c $< -o $@

$(LOADGEN_DIR)/bin/%.o: $(LOADGEN_DIR)/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(CPPFLAGS) -MMD -MP -c $< -o $@

-include $(DEPS)

# ---- housekeeping ---------------------------------------------------------
# clean removes only what we generated; distclean drops the bin/ dirs too.
clean:
	$(RM) $(OBJS) $(DEPS) $(LIB) $(CLIENT_BIN) $(SERVER_BIN) $(LOADGEN_BIN)

distclean:
	$(RM) -r $(LIB_DIR)/bin $(CLIENT_DIR)/bin $(SERVER_DIR)/bin $(LOADGEN_DIR)/bin

test:
	@echo "no tests yet"

help:
	@echo "targets:"
	@echo "  all        lib + client + server (default)"
	@echo "  lib        $(LIB)"
	@echo "  client     $(CLIENT_BIN)"
	@echo "  server     $(SERVER_BIN)"
	@echo "  tools      $(LOADGEN_BIN) (server load generator)"
	@echo "  clean      remove generated objects and artifacts"
	@echo "  distclean  remove the bin/ directories entirely"
