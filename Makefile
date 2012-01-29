TOPDIR	= $(CURDIR)

include $(TOPDIR)/Makefile.common

export TOPDIR

BINS	= tunnel
SUBDIRS	= p61 system

all: $(SUBDIRS) $(BINS)

p61:
	$(MAKE) -C $@

system:
	$(MAKE) -C $@

tunnel: startup_effect.o tunnel.o tunnel_res.o distortion.o distortion_opt.o \
	frame_tools.o p61/p61.lib system/system.lib
	$(CC) $(CFLAGS) $(LIBS) -o $@ $^

tunnel.o: tunnel.c
distortion.o: distortion.c distortion.h system/common.h
distortion_opt.o: distortion_opt.s distortion.h

clean:
	$(MAKE) -C p61 clean
	$(MAKE) -C system clean
	$(RM) *~ *.o $(BINS)

.PHONY: all p61 system

# vim: sw=8 ts=8
