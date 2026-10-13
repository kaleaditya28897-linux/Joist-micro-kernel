#!/bin/sh
# remote.sh - run a command in this working tree on the build and test VM
# (docs/testbed.md: joist-dev2, VM 114 on pvehome, since 2026-10-13 while
# pve1 is out of service; before that joist-dev, VM 113 on pve1).
#
# usage: tools/dev/remote.sh 'make -C model check'
#
# Copies the working tree as it is (uncommitted edits included, build
# directories and .git excluded) to ~/joist on the VM, then runs the command
# there from the top of the tree; its output and exit status come back.
# Edits, commits and pushes stay on the workstation. JOIST_DEV overrides the
# SSH host (default joist-dev2, an alias in ~/.ssh/config).
set -u
host=${JOIST_DEV:-joist-dev2}
top=$(cd "$(dirname "$0")/../.." && pwd)
[ $# -ge 1 ] || { echo "usage: $0 'command'" >&2; exit 2; }
# Contents decide what is copied (--checksum), and copied files get the time
# of the copy, not the workstation's (no -t): a file edited while an earlier
# remote build ran would otherwise arrive older than that build's output,
# and make would not rebuild it.
rsync -rlpD --checksum --delete --exclude .git --exclude build --exclude __pycache__ \
    --exclude 'mcp-server/joist-mcp' --exclude '*.o' "$top/" "$host:joist/" || exit 2
exec ssh -o BatchMode=yes "$host" "cd joist && $*"
