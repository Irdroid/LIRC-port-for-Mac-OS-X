## $Id: Makefile.am,v 1.1 2004/06/19 06:56:28 lirc Exp $

## Process this file with automake to produce Makefile.in

## this is so that Automake includes the C compiling definitions, and
## includes the source files in the distribution.
EXTRA_PROGRAMS = automake_dummy
automake_dummy_SOURCES = lirc_sasem.c lirc_sasem.h

## there is no *just* object file support in automake.  This is close enough
module_DATA = lirc_sasem.o

include ../Makefile.common
