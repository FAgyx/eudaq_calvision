bin_PROGRAMS = wavedump

wavedump_SOURCES = \
    fft.c \
    flash.c \
    keyb.c \
    spi.c \
    WaveDump.c \
    WDconfig.c \
    WDplot.c \
    X742CorrectionRoutines.c

wavedump_CPPFLAGS = \
    -I$(top_srcdir)/include

wavedump_CFLAGS = \
    $(PTHREAD_CFLAGS)

wavedump_LDADD = \
    $(PTHREAD_LIBS)
