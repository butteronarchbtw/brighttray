CC := clang
CFLAGS := -Wall -O2 -D_GNU_SOURCE $(shell pkg-config --cflags gtk+-3.0 libnotify) -lsystemd
LFLAGS := $(shell pkg-config --libs gtk+-3.0 libnotify)

SOURCES := $(wildcard src/*.c)
OBJECTS := $(patsubst %.c,%.o,$(SOURCES))
BINARY := ./brighttray

run: $(BINARY)
	$(BINARY)

$(BINARY): $(OBJECTS)
	$(CC) $(CFLAGS) $^ -o $@ $(LFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -o $@ -c $< $(LFLAGS)

clean:
	rm -f $(OBJECTS) $(BINARY)

