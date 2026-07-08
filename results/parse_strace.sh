#!/bin/bash
# parse_strace.sh <strace_file> <syscall_name>
# Prints the call count for the given syscall name.
# Handles both: "calls errors syscall" and "calls syscall" formats.

FILE=$1
SYSCALL=$2

awk -v sc="$SYSCALL" '
{
    # Last field is the syscall name
    name = $NF
    if (name == sc) {
        # If 6 fields: % time seconds usecs/call calls errors syscall
        # If 5 fields: % time seconds usecs/call calls syscall
        if (NF == 6) {
            print $4   # calls column when errors column present
        } else {
            print $4   # calls column (no errors)
        }
    }
}' "$FILE"
