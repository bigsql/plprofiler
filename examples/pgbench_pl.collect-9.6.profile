\set aid random(1, 100000 * :scale)
\set bid random(1, 1 * :scale)
\set tid random(1, 10 * :scale)
\set delta random(-5000, 5000)
SELECT pl_profiler_enable(true);
SELECT tpcb(:aid, :bid, :tid, :delta);
SELECT pl_profiler_collect_data();
SELECT pl_profiler_enable(false);
