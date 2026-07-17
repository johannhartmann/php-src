#!/usr/bin/env bash

set -Eeuo pipefail
IFS=$'\n\t'

SCRIPT_DIR="$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"

if [[ ${1:-} == -h || ${1:-} == --help ]]; then
    cat <<'EOF'
Configure a php-src W00 developer build (debug-nts by default).

Usage: configure-dev.sh [--profile PROFILE] [--force] [--print-build-dir]

This is the stable developer-facing wrapper around configure.sh. See
configure.sh --help for options and environment variables.
EOF
    exit 0
fi

exec "$SCRIPT_DIR/configure.sh" "$@"
