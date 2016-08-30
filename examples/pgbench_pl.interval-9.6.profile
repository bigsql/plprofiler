\set aid random(1, 100000 * :scale)
\set bid random(1, 1 * :scale)
\set tid random(1, 10 * :scale)
\set delta random(-5000, 5000)
SET plprofiler.enabled TO true;
SET plprofiler.save_interval TO 10;
SELECT tpcb(:aid, :bid, :tid, :delta);
