noinst_PROGRAMS = logtest
logtest_SOURCES = logtest.c

man8_MANS = cfsd.8

bin_PROGRAMS = cfsd create_cfs_db
cfsd_SOURCES = cfsd.c
create_cfs_db_SOURCES = create_cfs_db.c

TESTS = check_memdb
check_PROGRAMS = ${TESTS}
check_memdb_SOURCES = check_memdb.c
check_memdb_CFLAGS = $(AM_CFLAGS) @CHECK_CFLAGS@
check_memdb_LDADD = $(LDADD) @CHECK_LIBS@ 

noinst_LIBRARIES = libcfs.a
libcfs_a_SOURCES = 		\
	logger.h		\
	logger.c		\
	loop.h			\
	loop.c			\
	server.h		\
	server.c		\
	status.h		\
	status.c		\
	confdb.h		\
	confdb.c		\
	quorum.h		\
	quorum.c		\
	dcdb.h			\
	dcdb.c			\
	dfsm.c			\
	dfsm.h			\
	cfs-plug.h 		\
	cfs-plug.c 		\
	cfs-plug-link.c		\
	cfs-plug-func.c		\
	cfs-plug-memdb.h 	\
	cfs-plug-memdb.c 	\
	cfs-utils.h		\
	cfs-utils.c		\
	database.c		\
	memdb.h			\
	memdb.c

# add -lprofiler for google perftools
AM_CFLAGS =  -g -O -Wl,-z,relro -std=gnu99 -Wall -Werror -Wno-strict-aliasing $(COROSYNC_CFLAGS) $(SQLITE3_CFLAGS) $(FUSE_CFLAGS) $(QB_CFLAGS) $(GLIB_CFLAGS) $(LIBRRD_CFLAGS) 
LDADD = libcfs.a $(COROSYNC_LIBS) $(SQLITE3_LIBS) $(FUSE_LIBS) $(QB_LIBS) $(GLIB_LIBS) $(LIBRRD_LIBS) 

clean-local:
	make cleanup-docgen
	rm -f *~
