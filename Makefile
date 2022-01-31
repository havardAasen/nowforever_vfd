all: nowforever_vfd

prefix = /usr/local
exec_prefix = $(prefix)
bindir = $(exec_prefix)/bin
datarootdir = $(prefix)/share
mandir = $(datarootdir)/man
man1dir = $(mandir)/man1

CC = gcc
CFLAGS = -Wall -g -O
ALL_CFLAGS = -O2 -D_FORTITY_SOURCE=2 -DRTAPI -I/usr/include/linuxcnc -I/usr/include/modbus $(CFLAGS)
LDLIBS := -lmodbus -llinuxcnchal -lpthread -lm -lglib-2.0
LDFLAGS := -Wl,-z,now -Wl,-z,relro

nowforever_vfd: nowforever_vfd.o
	$(CC) -o $@ $^ $(LDFLAGS) $(LDLIBS)

%.o: %.c
	$(CC) $(ALL_CFLAGS) -o $@ -c $<

.PHONY: all install clean distclean uninstall

install: nowforever_vfd
	install -d -m 755 $(DESTDIR)$(bindir)
	install -d -m 755 $(DESTDIR)$(man1dir)
	install nowforever_vfd $(DESTDIR)$(bindir)/
	install -m 644 nowforever_vfd.1 $(DESTDIR)$(man1dir)/

clean:
	rm -f nowforever_vfd
	rm -f nowforever_vfd.o
	rm -f tags

distclean: clean

uninstall:
	-rm -f $(DESTDIR)$(bindir)/nowforever_vfd
	-rm -f $(DESTDIR)$(man1dir)/nowforever_vfd.1

TAGS: nowforever_vfd.c
	ctags $^

