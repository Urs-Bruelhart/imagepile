CC=gcc
CFLAGS=-O3 -g
#CFLAGS=-Og -g3
CFLAGS += $(CFLAGS_EXTRA)
BUILD_CFLAGS = -std=gnu99 -I. -D_FILE_OFFSET_BITS=64 -pipe -fstrict-aliasing
BUILD_CFLAGS += -Wall -Wextra -Wcast-align -Wstrict-aliasing -pedantic -Wstrict-overflow
#LDFLAGS=-s -Wl,--gc-sections
LDFLAGS=

prefix=/usr
exec_prefix=${prefix}
bindir=${exec_prefix}/bin
mandir=${prefix}/man
datarootdir=${prefix}/share
datadir=${datarootdir}
sysconfdir=${prefix}/etc

# MinGW needs this for printf() conversions to work
ifeq ($(OS), Windows_NT)
        BUILD_CFLAGS += -D__USE_MINGW_ANSI_STDIO=1
endif

ifdef DEBUG
BUILD_CFLAGS += -DDEBUG -g
endif

all: imagepile

imagepile: imagepile.o jody_hash.o
	$(CC) $(CFLAGS) $(LDFLAGS) $(BUILD_CFLAGS) -o imagepile imagepile.o jody_hash.o

#manual:
#	gzip -9 < imagepile.8 > imagepile.8.gz

.c.o:
	$(CC) -c $(CFLAGS) $(BUILD_CFLAGS) $<

clean:
	rm -f *.o *~ .*un~ imagepile imagepile.exe debug.log *.?.gz

distclean:
	rm -f *.o *~ .*un~ imagepile imagepile.exe debug.log *.?.gz *.pkg.tar.*

install: all
	install -D -o root -g root -m 0644 imagepile.8.gz $(DESTDIR)/$(mandir)/man8/imagepile.8.gz
	install -D -o root -g root -m 0755 -s imagepile $(DESTDIR)/$(bindir)/imagepile

package:
	+./chroot_build.sh
