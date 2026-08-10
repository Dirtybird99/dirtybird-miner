#!/usr/bin/env bash
killall -9 dirtybird-c-miner > /dev/null 2>&1
. h-manifest.conf

CUSTOM_LOG_BASEDIR=`dirname "$CUSTOM_LOG_BASENAME"`
[[ ! -d $CUSTOM_LOG_BASEDIR ]] && mkdir -p $CUSTOM_LOG_BASEDIR

# The config expansion stays unquoted on purpose -- it holds several flags that
# have to word-split into separate arguments. "$@" does not: quoting keeps a
# passed-through argument containing spaces as one argument.
LD_LIBRARY_PATH="./lib:${LD_LIBRARY_PATH}" ./dirtybird-c-miner $(< $CUSTOM_CONFIG_FILENAME) "$@" 2>&1 | tee --append ${CUSTOM_LOG_BASENAME}.log
