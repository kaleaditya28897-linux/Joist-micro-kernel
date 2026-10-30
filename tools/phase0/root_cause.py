#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Attribute each glibc clang error to a root cause (decision 0002, 2026-09-30).

usage: root_cause.py clang-glibc-errors-guard.csv > clang-glibc-root-causes.csv

Input is the output of extern_inline_guard.py. Adds a `root_cause` column.
Rules are applied in order and each is backed by a check recorded in
docs/phase0-data/README.md, stage 9:

  nested-function    class == nested (clang_errors.py): a GCC nested function
                     definition, or a use of its name in the same file.
  extern-inline-guard
                     glibc's include/features.h leaves __USE_EXTERN_INLINES
                     undefined when glibc itself is built with clang. Hurd-port
                     headers put ordinary declarations, inline definitions and
                     even #includes behind that guard, so under clang they vanish.
                     Matched when:
                       - guard_hidden == yes (the undeclared name is declared
                         inside a guarded block), or
                       - the name is HURD_CRITICAL_BEGIN/END: hurd/userlink.h:27
                         includes <hurd/signal.h> only under the guard, and
                         hurd.h reaches signal.h only through it, or
                       - "alias must point to a defined variable or function":
                         the alias targets (e.g. _hurd_port_locked_get,
                         hurd/hurd/port.h:91) are defined inside guarded blocks, or
                       - "subscript of pointer to function type 'int (int,
                         error_t)'": clang's did-you-mean replaced the hidden
                         _hurd_dtable with the function _hurd_fd_error, whose
                         type this is; a knock-on of the hidden declaration.
  vla-in-struct      "fields must have a constant size": a GCC extension clang
                     states it will never support. Real porting work.
  asm-label          "cannot apply asm label to function after its first use".
  generated-stdin    errors in <stdin>: glibc's generated offset programs
                     (gen-as-const), fed on stdin.
  nested-knock-on    class == nested-file: errors in a file that contains
                     nested functions, not attributable to a rule above.
  unattributed       everything else. Read these by hand.
"""
import csv, re, sys

rows = list(csv.DictReader(open(sys.argv[1])))
w = csv.DictWriter(sys.stdout, fieldnames=list(rows[0].keys()) + ['root_cause'])
w.writeheader()
for r in rows:
    m = r['message']
    name = (re.findall(r"'([A-Za-z_]\w*)'", m) or [''])[0]
    if r['class'] == 'nested':
        rc = 'nested-function'
    elif r['guard_hidden'] == 'yes' or name in ('HURD_CRITICAL_BEGIN', 'HURD_CRITICAL_END') \
            or 'alias must point to a defined variable or function' in m \
            or "subscript of pointer to function type 'int (int, error_t)'" in m:
        rc = 'extern-inline-guard'
    elif 'fields must have a constant size' in m:
        rc = 'vla-in-struct'
    elif 'cannot apply asm label' in m:
        rc = 'asm-label'
    elif r['file'] == '<stdin>':
        rc = 'generated-stdin'
    elif r['class'] == 'nested-file':
        rc = 'nested-knock-on'
    else:
        rc = 'unattributed'
    r['root_cause'] = rc
    w.writerow(r)
