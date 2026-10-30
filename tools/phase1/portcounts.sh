#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# portcounts.sh - port names held by every process on the running Hurd
# (Phase 1 work item 6; 0001 open item 4: how big a capability space gets).
# Run on the testbed as root (portinfo needs access to every task).
# Output CSV: pid,command,names,receive,send,send_once,port_set,dead_name
echo "pid,command,names,receive,send,send_once,port_set,dead_name"
for pid in $(ps -e -o pid= | sort -n); do
    cmd=$(ps -p "$pid" -o args= 2>/dev/null | awk 'NF {print $1; exit}' | tr ',' ';')
    portinfo "$pid" 2>/dev/null | awk -v p="$pid" -v c="$cmd" '
        /^ *[0-9]+:/ { n++ }
        / receive/   { r++ }
        / send[ ,]| send$/ && !/send-once/ { s++ }
        /send-once/  { o++ }
        /port-set/   { ps++ }
        /dead-name/  { d++ }
        END { if (n) printf "%s,%s,%d,%d,%d,%d,%d,%d\n", p, c, n, r, s, o, ps, d }'
done
