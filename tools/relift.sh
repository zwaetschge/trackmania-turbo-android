#!/bin/sh
# Re-lift all shards in parallel (only changed files are rewritten).
cd "$(dirname "$0")/.."
for i in 0 1 2 3 4 5 6 7; do
  PCRECOMP=$PWD/third_party/pcrecomp .venv/bin/python -u tools/run_lift.py lift ref/TrackmaniaTurbo.exe work/catalog.json gen --shard $i/8 > work/lift_s$i.log 2>&1 &
done
wait
.venv/bin/python -c "
import json,glob
c=sum(json.load(open(p)).get('files_changed',0) for p in glob.glob('gen/lift_stats_s*.json')); print('files changed:', c)"
