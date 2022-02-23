SHELL = /bin/sh

CC = gcc
CFLAGS = -Wall -g -O
ALL_CFLAGS = -O2 -D_FORTITY_SOURCE=2 -DRTAPI \
             -I/usr/include/linuxcnc \
             -I/usr/include/modbus \
             $(CFLAGS)
LDLIBS := -lmodbus -llinuxcnchal -lm
LDFLAGS := -Wl,-z,now -Wl,-z,relro

BIN = nowforever_vfd
SRCS = nowforever_vfd.c
OBJS = $(patsubst %.c,%.o, $(SRCS))

prefix = /usr/local
exec_prefix = $(prefix)
bindir = $(exec_prefix)/bin
datarootdir = $(prefix)/share
mandir = $(datarootdir)/man
man1dir = $(mandir)/man1

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(ALL_CFLAGS) $^ -o $@ $(LDFLAGS) $(LDLIBS)

%.o: %.c
	$(CC) $(ALL_CFLAGS) -c $< -o $@

install: $(BIN)
	install -d -m 755 $(DESTDIR)$(bindir)
	install -d -m 755 $(DESTDIR)$(man1dir)
	install $(BIN) $(DESTDIR)$(bindir)/
	install -m 644 nowforever_vfd.1 $(DESTDIR)$(man1dir)/

clean:
	$(RM) $(BIN)
	$(RM) $(OBJS)

distclean: clean
	$(RM) tags

uninstall:
	$(RM) $(DESTDIR)$(bindir)/$(BIN)
	$(RM) $(DESTDIR)$(man1dir)/nowforever_vfd.1

TAGS: $(SRCS)
	ctags $^

.PHONY: all install clean distclean uninstall
