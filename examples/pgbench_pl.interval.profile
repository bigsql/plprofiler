\set nbranches :scale
\set ntellers 10 * :scale
\set naccounts 100000 * :scale
\setrandom aid 1 :naccounts
\setrandom bid 1 :nbranches
\setrandom tid 1 :ntellers
\setrandom delta -5000 5000
SET plprofiler.enabled TO true;
SET plprofiler.save_interval TO 10;
SELECT tpcb(:aid, :bid, :tid, :delta);
