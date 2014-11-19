CC=gcc
#CFLAGS=-Os -flto -ffunction-sections -fdata-sections -fno-unwind-tables -fno-asynchronous-unwind-tables
CFLAGS=-O2 -flto
#CFLAGS=-Og -g3
BUILD_CFLAGS=-std=gnu99 -I. -D_FILE_OFFSET_BITS=64 -pipe -Wall -pedantic
LDFLAGS=-s -Wl,--gc-sections
#LDFLAGS=

prefix=/usr
exec_prefix=${prefix}
bindir=${exec_prefix}/bin
mandir=${prefix}/man
datarootdir=${prefix}/share
datadir=${datarootdir}
sysconfdir=${prefix}/etc

all: imagepile

imagepile: imagepile.o
	$(CC) $(CFLAGS) $(LDFLAGS) $(BUILD_CFLAGS) -o imagepile imagepile.o

#manual:
#	gzip -9 < imagepile.8 > imagepile.8.gz

.c.o:
	$(CC) -c $(BUILD_CFLAGS) $(FUSE_CFLAGS) $(CFLAGS) $<

clean:
	rm -f *.o *~ .*un~ imagepile debug.log *.?.gz

distclean:
	rm -f *.o *~ .*un~ imagepile debug.log *.?.gz *.pkg.tar.*

install: all
	install -D -o root -g root -m 0644 imagepile.8.gz $(DESTDIR)/$(mandir)/man8/imagepile.8.gz
	install -D -o root -g root -m 0755 -s imagepile $(DESTDIR)/$(bindir)/imagepile

package:
	+./chroot_build.sh
