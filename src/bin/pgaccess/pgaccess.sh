#!/bin/sh

PATH_TO_WISH='@WISH@'
PGACCESS_HOME='@PGACCESSHOME@'
PGLIB='@PGLIB@'
PGPORT="${PGPORT:-@DEF_PGPORT@}"

export PATH_TO_WISH
export PGACCESS_HOME
export PGLIB
export PGPORT

exec "${PATH_TO_WISH}" "${PGACCESS_HOME}/main.tcl" "$@"
