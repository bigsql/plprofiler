PL/pgSQL functions for pgbench_pl
=================================

```SQL
-- ----------------------------------------------------------------------
-- pgbench_pl.sql
--
--	PL/pgSQL functions implementing the pgbench transaction.
-- ----------------------------------------------------------------------

CREATE OR REPLACE FUNCTION
tpcb(par_aid integer, par_bid integer, par_tid integer, par_delta integer)
RETURNS integer AS
$$
DECLARE
    var_abalance	integer;
BEGIN
	var_abalance = tpcb_upd_accounts(par_aid, par_delta);
	PERFORM tpcb_upd_tellers(par_tid, par_delta);
	PERFORM tpcb_upd_branches(par_bid, par_delta);
	PERFORM tpcb_ins_history(par_aid, par_tid, par_bid, par_delta);
	RETURN var_abalance;
END;
$$
LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION
tpcb_upd_accounts(par_aid integer, par_delta integer)
RETURNS integer AS
$$
BEGIN
	UPDATE pgbench_accounts SET abalance = abalance + par_delta
		WHERE aid = par_aid;
	RETURN tpcb_fetch_abalance(par_aid);
END;
$$
LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION
tpcb_fetch_abalance(par_aid integer)
RETURNS integer AS
$$
DECLARE
	var_abalance	integer;
BEGIN
	-- SELECT abalance INTO var_abalance
	-- 	FROM pgbench_accounts WHERE aid = par_aid;
	-- RETURN var_abalance;
	RETURN abalance FROM pgbench_accounts WHERE aid = par_aid;
END;
$$
LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION
tpcb_upd_tellers(par_tid integer, par_delta integer)
RETURNS void AS
$$
BEGIN
	UPDATE pgbench_tellers SET tbalance = tbalance + par_delta
		WHERE tid = par_tid;
END;
$$
LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION
tpcb_upd_branches(par_bid integer, par_delta integer)
RETURNS void AS
$$
BEGIN
	UPDATE pgbench_branches SET bbalance = bbalance + par_delta
		WHERE bid = par_bid;
END;
$$
LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION
tpcb_ins_history(par_aid integer, par_tid integer, par_bid integer, par_delta integer)
RETURNS void AS
$$
BEGIN
    INSERT INTO pgbench_history (tid, bid, aid, delta, mtime)
		VALUES (par_tid, par_bid, par_aid, par_delta, CURRENT_TIMESTAMP);
END;
$$
LANGUAGE plpgsql;
```
