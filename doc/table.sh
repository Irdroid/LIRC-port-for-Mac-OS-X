#! /bin/sh

HWDB="${1:-/dev/null}"
HEURISTIC=`mktemp`

write_heuristic="no"
cat ../configure.ac | while read REPLY; do
    if echo $REPLY|grep "START HARDWARE HEURISTIC" >/dev/null; then
	write_heuristic="yes"
	continue;
    fi
    if echo $REPLY|grep "END HARDWARE HEURISTIC" >/dev/null; then
	write_heuristic="no"
    fi
    if test "${write_heuristic}" = "yes"; then
	if echo $REPLY|grep "AC_DEFINE">/dev/null; then
	    continue;
	fi
	if echo $REPLY|grep "^echo">/dev/null; then
	    continue;
	fi
	if echo $REPLY|grep "exit 1">/dev/null; then
	    continue;
	fi
	echo $REPLY >>${HEURISTIC}
    fi
done

cat << HWDB_HEADER > "${HWDB}"
# LIRC - Hardware DataBase
#
# This file lists all the remote controls supported by LIRC
# in a parseable form.
#
# The format is:
#
# [remote controls type]
# description;driver;lirc driver;HW_DEFAULT;lircd_conf;
#
#
HWDB_HEADER

cat html-source/head.html

echo "<table border=\"1\">"
echo "<tr><th>Hardware</th><th>configure --with-driver option</th><th>Required LIRC kernel modules</th><th>lircd driver</th><th>default lircd and lircmd config files</th></tr>"
grep ".*: \(\".*\"\)\|@" ../setup.data | while read REPLY; do
    #echo $REPLY

    if echo $REPLY|grep ": @any" >/dev/null; then
	continue;
    fi
    
    if echo $REPLY|grep ": @" >/dev/null; then
	entry=`echo $REPLY|sed --expression="s/.*: \(@.*\)/\1/"`
	desc=`grep "${entry}:" ../setup.data|sed --expression="s/.*\"\(.*\)\".*/\1/"`
	echo "" >> "${HWDB}"
	echo "[$desc]" >> "${HWDB}"
	echo "<tr><th colspan=\"5\"><a name=\"${entry}\">${desc}</a></th></tr>"
	continue;
    fi
    
    desc=`echo $REPLY|sed --expression="s/.*\"\(.*\)\".*/\1/"`
    driver=`echo $REPLY|sed --expression="s/\(.*\):.*/\1/"`
    
    if test "$driver" = "any" -o "$driver" = "none"; then
	continue;
    fi

    if echo $driver|grep @ >/dev/null; then
	echo "<tr><th colspan=\"5\"><a href=\"#${driver}\">${desc}</a></th></tr>"
	true;
    else
	. ${HEURISTIC}
	if ! echo "${lirc_driver}"|grep lirc_dev>/dev/null; then
	    lirc_driver="none"
	fi
	echo -n "<tr><td>${desc}</td><td align=\"center\">"
	
	if test -f html-source/${driver}.html; then
	    driver_doc=${driver}
	elif test -f `echo html-source/${driver}.html|sed --expression="s/_/-/g"`; then
	    driver_doc=`echo $driver|sed --expression="s/_/-/g"`
	else
	    driver_doc=""
	fi

	if test "$driver_doc" != ""; then
	    echo -n "<A HREF=\"${driver_doc}.html\">${driver}</A>"
	else
	    echo -n "${driver}"
	fi
	echo "</td><td align=\"center\">${lirc_driver}</td><td align=\"center\">${HW_DEFAULT#???}</td><td>${lircd_conf}<br>${lircmd_conf}</td></tr>"
	echo "${desc};${driver};${lirc_driver};${HW_DEFAULT};${lircd_conf};" >> "${HWDB}"
    fi
done
echo "</table>"

cat html-source/foot.html
rm ${HEURISTIC}
