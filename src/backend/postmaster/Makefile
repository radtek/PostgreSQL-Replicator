#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for src/backend/postmaster
#
# IDENTIFICATION
#    $PostgreSQL$
#
#-------------------------------------------------------------------------

subdir = src/backend/postmaster
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS = autovacuum.o bgwriter.o fork_process.o pgarch.o pgstat.o postmaster.o \
	   syslogger.o walwriter.o replication.o

include $(top_srcdir)/src/backend/common.mk
