INCS=-I../cdebconf/src/
LDOPTS=-L../cdebconf/src/ -ldebconf
PREFIX=$(DESTDIR)/usr/
CFLAGS=-Wall  -Os -fomit-frame-pointer
INSTALL=install
STRIPTOOL=strip
STRIP = $(STRIPTOOL) --remove-section=.note --remove-section=.comment

all: netcfg
	$(STRIP) netcfg
	size netcfg

netcfg: netcfg.c 
	$(CC) $(CFLAGS) netcfg.c -o netcfg $(INCS) $(LDOPTS)

install:
	mkdir -p $(PREFIX)/bin/
	$(INSTALL) netcfg $(PREFIX)/bin/

clean:
	rm -f *.o netcfg
