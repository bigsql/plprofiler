#!/bin/sh

PGDATABASE=pgbench_plprofiler
export PGDATABASE

# ----
# Initialize the pgbench schema itself.
# ----
pgbench -i -s 10 -F90

# ----
# Create the stored procedures implementing the TPC-B transaction.
# ----
psql <pgbench_pl.sql

# ----
# We now create a problem in the database.
#
#	This is based on a real world problem found in a customer database
#	with the help of the plprofiler. Please see the plprofiler documentation
#	for details.
# ----
psql -e <<_EOF_

DROP EXTENSION IF EXISTS plprofiler;
CREATE EXTENSION plprofiler;

DROP SEQUENCE IF EXISTS pgbench_category_seq;
CREATE SEQUENCE pgbench_category_seq;

DROP TABLE IF EXISTS pgbench_accounts_new;
CREATE TABLE pgbench_accounts_new (
    category	integer default (nextval('pgbench_category_seq') / 20),
	aid			integer NOT NULL,
	bid			integer,
	abalance	integer,
	filler		character(1000)
)
WITH (FILLFACTOR = 90);

INSERT INTO pgbench_accounts_new
		(aid, bid, abalance, filler)
	SELECT aid, bid, abalance,
			'11111111111111111111111111111111111111111111111111' ||
			'11111111111111111111111111111111111111111111111111' ||
			'22222222222222222222222222222222222222222222222222' ||
			'22222222222222222222222222222222222222222222222222' ||
			'33333333333333333333333333333333333333333333333333' ||
			'33333333333333333333333333333333333333333333333333' ||
			'44444444444444444444444444444444444444444444444444' ||
			'44444444444444444444444444444444444444444444444444' ||
			'55555555555555555555555555555555555555555555555555' ||
			'55555555555555555555555555555555555555555555555555'
	FROM pgbench_accounts;

DROP TABLE pgbench_accounts;
ALTER TABLE pgbench_accounts_new RENAME TO pgbench_accounts;

ALTER TABLE pgbench_accounts ADD CONSTRAINT pgbench_accounts_pkey
	PRIMARY KEY (category, aid);

VACUUM FULL ANALYZE pgbench_accounts;

_EOF_
