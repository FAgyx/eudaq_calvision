SUBDIRS = src

dist_noinst_HEADERS = \
	include/WDconfig.h \
	include/WDplot.h \
	include/WaveDump.h \
	include/X742CorrectionRoutines.h \
	include/fft.h \
	include/flash.h \
	include/flash_opcodes.h \
	include/keyb.h \
	include/spi.h

# some hardcoded paths point to /etc/wavedump
wavedumpconfdir = /etc/wavedump

dist_wavedumpconf_DATA = \
	WaveDumpConfig.txt \
	WaveDumpConfig_X740.txt \
	WaveDumpConfig_X742.txt

install-data-hook:
	chmod a+rw $(DESTDIR)$(wavedumpconfdir)/WaveDumpConfig.txt
	chmod a+rw $(DESTDIR)$(wavedumpconfdir)/WaveDumpConfig_X740.txt
	chmod a+rw $(DESTDIR)$(wavedumpconfdir)/WaveDumpConfig_X742.txt
