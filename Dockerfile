# PostgreSQL 18.6 (versión exigida por la rúbrica) + extensión ISAM compilada dentro.
ARG PG_VERSION=18.6
FROM postgres:${PG_VERSION}

RUN apt-get update \
 && apt-get install -y --no-install-recommends build-essential postgresql-server-dev-18 \
 && rm -rf /var/lib/apt/lists/*

ENV PG_CONFIG=/usr/lib/postgresql/18/bin/pg_config

WORKDIR /build
COPY Makefile isam.control ./
COPY src ./src
COPY sql ./sql
COPY test ./test
# make test-core corre las pruebas del núcleo (sin PostgreSQL) durante el build
RUN make && make install && make test-core

COPY docker/00_extension.sql /docker-entrypoint-initdb.d/00_extension.sql
COPY scripts /scripts
COPY test/test_pg.sql /scripts/test_pg.sql
