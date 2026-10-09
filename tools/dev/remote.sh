#!/bin/sh
# remote.sh - run a command in this working tree on the build and test VM
# (joist-dev: VM 113 on pve1, docs/testbed.md).
#
# usage: tools/dev/remote.sh 'make -C model check'
#
# Copies the working tree as it is (uncommitted edits included, build
# directories and .git excluded) to ~/joist on the VM, then runs the command
# there from the top of the tree; its output and exit status come back.
# Edits, commits and pushes stay on the workstation. JOIST_DEV overrides the
# SSH host (default joist-dev, an alias in ~/.ssh/config).
set -u
host=${JOIST_DEV:-joist-dev}
top=$(cd "$(dirname "$0")/../.." && pwd)
[ $# -ge 1 ] || { echo "usage: $0 'command'" >&2; exit 2; }
rsync -a --delete --exclude .git --exclude build --exclude __pycache__ \
    --exclude 'mcp-server/joist-mcp' --exclude '*.o' "$top/" "$host:joist/" || exit 2
exec ssh -o BatchMode=yes "$host" "cd joist && $*"
