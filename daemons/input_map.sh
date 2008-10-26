#! /bin/sh

TYPES="KEY BTN"
file=${1:-/usr/include/linux/input.h}

for type in $TYPES; do
	grep "^#define ${type}_" < $file|sort|while read; do
		sed --expression="s/^#define \([^ 	]*\)\(.*\)/{\"\1\", \2},/"|grep -v "KEY_HANGUEL\|KEY_MIN_INTERESTING"
	done
done

