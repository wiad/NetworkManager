if ENABLE_TESTS

AM_CPPFLAGS = \
	-I$(top_srcdir)/shared \
	-I$(top_builddir)/shared \
	-I$(top_srcdir)/libnm-core \
	-I$(top_builddir)/libnm-core \
	-I$(top_srcdir)/src \
	-I$(top_srcdir)/src/settings \
	-I$(srcdir)/../ \
	-DG_LOG_DOMAIN=\""NetworkManager-ifupdown"\" \
	-DNETWORKMANAGER_COMPILATION=NM_NETWORKMANAGER_COMPILATION_INSIDE_DAEMON \
	$(GLIB_CFLAGS) \
	-DTEST_ENI_DIR=\"$(abs_srcdir)\"

noinst_PROGRAMS = test-ifupdown

test_ifupdown_SOURCES = \
	test-ifupdown.c \
	../interface_parser.c \
	../parser.c

test_ifupdown_LDADD = \
	$(top_builddir)/src/libNetworkManager.la

@VALGRIND_RULES@
TESTS = test-ifupdown

endif

EXTRA_DIST = \
	test1 test2 test3 test4 test5 test6 test7 test8 test9 test11 test12 \
	test13 test14 test15 test16 test17-wired-static-verify-ip4 \
	test18-wired-static-verify-ip6 test19-wired-static-verify-ip4-plen \
	test20-source-stanza test20-source-stanza.eth0 test20-source-stanza.eth1
