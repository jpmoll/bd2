# Compilar e instalar:   make && make install
# Probar el núcleo solo: make test-core
EXTENSION  = isam
DATA       = sql/isam--1.0.sql
MODULE_big = isam
OBJS       = src/isam_core.o src/isam_am.o
PG_CPPFLAGS = -I$(CURDIR)/src
PG_CFLAGS = -Wno-declaration-after-statement

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

test/test_core: test/test_core.c src/isam_core.c src/isam_core.h
	$(CC) -O2 -Wall -Wextra -o $@ test/test_core.c src/isam_core.c

test-core: test/test_core
	./test/test_core
