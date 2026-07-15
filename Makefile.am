include $(top_srcdir)/build/modmake.rulesam

MODNAME = mod_broadcast

mod_LTLIBRARIES = mod_broadcast.la

mod_broadcast_la_SOURCES = \
	mod_broadcast.c \
	broadcast_core.c \
	broadcast_member.c \
	broadcast_producer.c \
	broadcast_listener.c \
	broadcast_speaker.c \
	broadcast_record.c \
	broadcast_events.c \
	broadcast_api.c \
	broadcast_config.c

mod_broadcast_la_CFLAGS  = $(AM_CFLAGS)
mod_broadcast_la_LDFLAGS = -avoid-version -module -no-undefined -shared
mod_broadcast_la_LIBADD  = $(switch_builddir)/libfreeswitch.la

$(am_mod_broadcast_la_OBJECTS): mod_broadcast.h

EXTRA_DIST = \
	conf/autoload_configs/broadcast.conf.xml \
	test/README.txt \
	test/run_sipp_examples.sh \
	test/sipp/bcast_speaker.xml \
	test/sipp/bcast_listener.xml \
	README.md \
	TESTING.md \
	HANDOFF.md \
	examples/broadcast_examples.xml \
	examples/broadcast_coordinator.lua

install-data-local:
	$(MKDIR_P) $(DESTDIR)$(confdir)/autoload_configs
	$(INSTALL_DATA) $(srcdir)/conf/autoload_configs/broadcast.conf.xml $(DESTDIR)$(confdir)/autoload_configs/broadcast.conf.xml

uninstall-local:
	rm -f $(DESTDIR)$(confdir)/autoload_configs/broadcast.conf.xml
