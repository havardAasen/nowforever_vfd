default: nowforever_vfd

CC = gcc
CFLAGS := -Wall -D_FORTITY_SOURCE=2 -O2 -DRTAPI -I/usr/include/linuxcnc -I/usr/include/modbus
LDLIBS := -lmodbus -llinuxcnchal -lpthread -lm -lglib-2.0
LDFLAGS := -Wl,-z,now -Wl,-z,relro

nowforever_vfd: nowforever_vfd.o
	$(CC) -o $@ $^ $(LDFLAGS) $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -o $@ -c $<

.PHONY: clean
clean:
	rm -f nowforever_vfd
	rm -f nowforever_vfd.o
	rm -f tags

TAGS: nowforever_vfd.c
	ctags $^

