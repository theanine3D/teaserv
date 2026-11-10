#!/bin/sh

set -e

EXECUTABLE="$1"
if [ -z "$EXECUTABLE" ]; then
  EXECUTABLE="./teaserv"
fi

$EXECUTABLE --config=tests/config.json check-client-functions
