#!/usr/bin/env bash
# export-member.sh <karrar|abdulwadood|talal|husain>
# Produces dist/minish-<member>.tar.gz (full tree + PORTION.md) and
# dist/minish-<member>.patch (git history touching only their files).
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
m=${1:?member name}
case $m in
  karrar)      who="Karrar Alqallaf";    tag="parser"; paths="src/parser docs/01-grammar.md"; doc="docs/01-grammar.md"
               desc="Tokenizer and parser (quotes, | < > >> &), prompt loop and line reader, syntax errors, size limits, integration (Makefile, shared header)";;
  abdulwadood) who="Abdul Wadood Syed";  tag="exec";   paths="src/exec docs/04-error-handling.md"; doc="docs/04-error-handling.md"
               desc="fork/execvp/dispatch, command-not-found and exec error classes (126/127), cd and exit built-ins, built-in table, failure-recovery policy";;
  talal)       who="Talal Alshammari";   tag="io";     paths="src/io docs/03-pipelines-redirection.md"; doc="docs/03-pipelines-redirection.md"
               desc="Redirections with open/dup2, multi-stage pipelines, closing every unused descriptor in parent and children, pipeline exit status, descriptor-leak tests";;
  husain)      who="Husain Althagafi";   tag="jobs";   paths="src/jobs docs/02-job-control.md tests/pty_tests.py"; doc="docs/02-job-control.md"
               desc="Job table, & jobs, SIGCHLD reaping with signal masking, process groups and terminal handoff, Ctrl+C/Ctrl+Z, jobs/fg/bg, pty test suite";;
  *) echo "unknown member: $m" >&2; exit 2;;
esac
mkdir -p "$ROOT/dist"
stage=$(mktemp -d)
git -C "$ROOT" archive --format=tar --prefix="minish-$m/" HEAD | tar -x -C "$stage"
{
  echo "# Your portion of MiniSh: $who"
  echo
  echo "$desc."
  echo
  echo "## Files you own"
  echo
  for p in $paths; do
      if [ -d "$ROOT/$p" ]; then find "$ROOT/$p" -type f | sed "s#^$ROOT/#- #"; else echo "- $p"; fi
  done
  echo
  echo "## Interface you implement"
  echo
  echo "See the block marked [$tag] in include/minish.h. Other modules call only those functions."
  echo
  echo "## Build and test your part"
  echo
  echo '```'
  echo "make && make test"
  echo "git log --oneline --grep='\\[$tag\\]'      # commits for this module"
  echo '```'
  echo
  echo "Design document to present: $doc"
} > "$stage/minish-$m/PORTION.md"
tar -czf "$ROOT/dist/minish-$m.tar.gz" -C "$stage" "minish-$m"
# shellcheck disable=SC2086
git -C "$ROOT" log -p --reverse -- $paths > "$ROOT/dist/minish-$m.patch"
rm -rf "$stage"
echo "wrote dist/minish-$m.tar.gz and dist/minish-$m.patch"
