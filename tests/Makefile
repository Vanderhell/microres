CC ?= cc
CPPFLAGS ?=
CFLAGS ?=
LDFLAGS ?=

WARNINGS = -Wall -Wextra -Wpedantic -Werror
BASE_CFLAGS = -std=c99
INCLUDES = -I../include

TEST_SOURCES = ../src/mres.c test_all.c
HARNESS_SOURCES = harness_selftest.c

.PHONY: all clean selftest

all: test_all

test_all: $(TEST_SOURCES) ../include/mres.h ../include/mres_config.h
	$(CC) $(CPPFLAGS) $(BASE_CFLAGS) $(WARNINGS) $(CFLAGS) $(INCLUDES) $(TEST_SOURCES) $(LDFLAGS) -o $@

selftest: harness_selftest

harness_selftest: $(HARNESS_SOURCES)
	$(CC) $(CPPFLAGS) $(BASE_CFLAGS) $(WARNINGS) $(CFLAGS) $(HARNESS_SOURCES) $(LDFLAGS) -o $@

clean:
	$(RM) test_all harness_selftest
