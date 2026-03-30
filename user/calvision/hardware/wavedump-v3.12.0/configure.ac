#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

# Tested with autoconf 2.69, released in 2012
AC_PREREQ([2.69])
AC_INIT(wavedump, [m4_esyscmd_s([echo $VERSION])], support.computing@caen.it)
AM_INIT_AUTOMAKE([-Wall foreign subdir-objects tar-ustar])

AC_PROG_CC
AC_PROG_CC_C99
AC_PROG_INSTALL
AC_DEFINE(LINUX,[1],"Define LINUX")

# Checks for standard libraries
AC_SEARCH_LIBS([sqrt], [m])
AX_PTHREAD([], [AC_MSG_ERROR([pthread required.])])

# Checks for CAEN libraries
dgtz_not_found_msg="CAENDigitizer >= 2.16.2 must be installed on your system."
comm_not_found_msg="CAENComm must be installed on your system."
AC_CHECK_HEADER([CAENDigitizer.h], [], [AC_MSG_ERROR($dgtz_not_found_msg)])
AC_CHECK_HEADER([CAENDigitizerType.h], [], [AC_MSG_ERROR($dgtz_not_found_msg)])
AC_CHECK_SYMBOL([CAEN_DGTZ_USB_A4818], [CAENDigitizerType.h], [], [AC_MSG_ERROR($dgtz_not_found_msg)])
AC_SEARCH_LIBS([CAEN_DGTZ_LoadSAMCorrectionData], [CAENDigitizer], [], [AC_MSG_ERROR($dgtz_not_found_msg)])
AC_SEARCH_LIBS([CAENComm_Write32], [CAENComm], [], [AC_MSG_ERROR($comm_not_found_msg)])

# Checks for programs
AC_CHECK_PROG(GNUPLOT, gnuplot, "yes", "no")
if test $GNUPLOT == "no"; then
  AC_MSG_ERROR(
    *****************************************************************
    GnuPlot not found. Please install it and retry.
    *****************************************************************
  )
fi

# Checks for library functions.
AC_CONFIG_FILES([
	Makefile
	src/Makefile
])

AC_OUTPUT
