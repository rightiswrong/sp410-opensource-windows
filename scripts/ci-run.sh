#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The sp410-cups-driver and sp410-opensource-windows contributors.
#
# ci-run.sh - run a command in CI; if it fails, surface the last lines of its
# output as a GitHub Actions error annotation so the failure is visible on the
# run summary page (and through the checks API) without opening raw logs.
#
#   scripts/ci-run.sh "<title>" <command> [args...]

set -uo pipefail
title=$1; shift
log=$(mktemp)
"$@" 2>&1 | tee "$log"
rc=${PIPESTATUS[0]}
if [[ $rc -ne 0 ]]; then
  # Annotations are single-line; GitHub decodes %0A as a newline.
  tail -n 80 "$log" | sed -e 's/%/%25/g' -e 's/\r/%0D/g' | awk 'BEGIN{ORS="%0A"} {print}' > "$log.tail"
  echo "::error title=${title} (exit ${rc})::$(cat "$log.tail")"
fi
rm -f "$log" "$log.tail"
exit "$rc"
