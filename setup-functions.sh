setup_seq_init() {
	local INDEX=1
	local N
	for N in 1 2 3 4 5 6 7 8 9 0 a b c d e f g h i j k l m n o p q r s \
		t u v w x y z A B C D E F G H I J K L M N O P Q R S T U V W \
		X Y Z
	do
		LIRC_SETUP_NICE_SEQ[$INDEX]="$N"
		INDEX=$(expr $INDEX + 1)
	done
}

nice_seq() {
	echo ${LIRC_SETUP_NICE_SEQ[$1]}
}

query_setup_data() {
	local CONFIG_TEXT="$1"
	local TITLE="$2"
	local QUESTION="$3"
	local DEVICE="$4"
	unset NAMES; local NAMES
	unset SEQ; local SEQ
	local INDEXES=""
	local NAME
	TEMP=$(mktemp /tmp/lirc.XXXXXXXXXX)
	FULL_ENTRY=$(sed -n \
		-e ': start' \
		-e 's/\\[ 	]*$//' \
		-e 't more' \
		-e 'b cont' \
		-e ': more' \
		-e 'N' \
		-e 'b start' \
		-e ': cont' \
		-e 's/\n[ 	]*//g' \
		-e '/^[ 	]*#/d' \
		-e "/^$QUESTION:.* $DEVICE\>\([^-]\|$\)/,/^ *$/p" \
		${SETUP_DATA})
	HACKED_ENTRY=$(echo "$FULL_ENTRY" | sed \
		-e "/^$QUESTION:/d" \
		-e '/^ *$/d' \
		-e 's/^[ 	]*//' \
		-e 's/:[ 	]*/ DESC=/' \
		-e 's/[ 	]*$//' \
		-e '/^TITLE=/p' \
		-e '/^TITLE=/d' \
		-e '/^CONFIG_TEXT=/p' \
		-e '/^CONFIG_TEXT=/d' \
		-e 's/^/NAME=/' )
	echo "$HACKED_ENTRY" > "$TEMP"
	exec < "$TEMP"
	MENU=""
	N=0
	DEFAULT=""
	read LINE
	while [ -n "$LINE" ]
	do
		eval "$LINE"
		if
			! expr "$LINE" : 'NAME' > /dev/null 2>&1
		then
			read LINE
			continue
		fi
		if [ -z "$DESC" ]; then
			DEFAULT="$NAME"
		else
			N=$(expr $N + 1)
			SEQ=$(nice_seq $N)
			INDEXES="$INDEXES $SEQ"
			MENU="$MENU \\
			$SEQ \"$DESC\""
			if
				! expr "$NAME" : '@' > /dev/null 2>&1
			then
				eval "SEQ_$NAME"="\"$SEQ\""
			fi
			eval "NAME_$SEQ"="\"$NAME\""
		fi
		read LINE
	done
	if [ $N -eq 0 ] && [ -z "$DEFAULT" ]; then
		#Nothing found assume 'none' and return error.
		echo none
		rm $TEMP
		return 1
	elif [ $N -le 1 ]; then
		#param_type or default_param entry
		echo $DEFAULT
		rm $TEMP
		return
	elif [ $N -gt 12 ]; then
		#More than 12 items at once don't look good.
		N=12
	fi
	HEIGHT=$(expr $N + 7)
	WIDTH=74
	eval DEFAULT_ITEM="\$SEQ_$DEFAULT"
	{
	local INDENT="$INDENT    "
	cat <<EOF 
${INDENT}dialog  --clear --backtitle "\$BACKTITLE" \\
${INDENT}        --title "$TITLE" \\
${INDENT}        --menu "\$$CONFIG_TEXT" $HEIGHT $WIDTH $N \\
${INDENT}          $MENU 2> \$TEMP
${INDENT}if test "\$?" = "0"; then
EOF
	{
	local INDENT="$INDENT    "
	echo "${INDENT}{"
	echo "${INDENT}set \`cat \$TEMP\`"
	echo "${INDENT}if false; then :"
	for N in $INDEXES
	do
		eval NAME="\$NAME_$N"
		echo -n "${INDENT}"'elif test "$1" = "'"$N"'"; then '
		if 
			expr "$NAME" : '@' > /dev/null 2>&1
		then
			echo
			(query_setup_data "$CONFIG_TEXT" \
				"$TITLE" "$QUESTION" "$NAME")
		else
			{
			local PARAM_TYPE
			local DEF_PARAM
			local PARAM_TYPE_RET
			PARAM_TYPE=$(get_param_type "$NAME")
			PARAM_TYPE_RET=$?
			DEF_PARAM=$(get_default_param "$NAME")
			if [ "$PARAM_TYPE_RET" != '0' ]; then
				if [ "$DEF_PARAM" = "none" ]; then
					echo $NAME: There is no param type, \
						but default parameter is \
						'none', should be OK >&2
				else
					echo $NAME: There is no param type, \
						and default parameter is \
						$DEF_PARAM >&2
					echo "	something may be WRONG" >&2
				fi
			fi
			echo -n 'LIRC_DRIVER='"${NAME};	" 
			if [ "$(expr length "$NAME")" -lt 8 ]; then
				echo -n "	"
			fi
			echo 	'DRIVER_PARAMETER='"$DEF_PARAM;" \
				'DRIVER_PARAM_TYPE='"$PARAM_TYPE;"
			}
		fi
	done
	echo "${INDENT}fi"
	echo "${INDENT}}"
	echo "${INDENT}else"
	echo "${INDENT}    return;"
	echo "${INDENT}fi;"
	}
	}
	rm $TEMP
}
gen_hw_type_menu()
{
	local TITLE="Select your driver"
	local CONFIG_TEXT="CONFIG_DRIVER_TEXT"
	local QUESTION="hw_menu_entry"
	INDENT=""

	query_setup_data "$CONFIG_TEXT" "$TITLE" \
				"$QUESTION" "@any"
	#echo '	echo $NAME'
}
get_default_param()
{
	local TITLE="Specify I/O base address and IRQ of your hardware"
	local CONFIG_TEXT="SET_PORT_TEXT"
	local QUESTION="default_param"
	local DEVICE="$1"

	query_setup_data "$CONFIG_TEXT" "$TITLE" \
				"$QUESTION" "$DEVICE"
}
get_param_type()
{
	local TITLE=""
	local CONFIG_TEXT="CONFIG_TEXT"
	CONFIG_TEXT="$CONFIG_TEXT combination, or enter costum values"
	local QUESTION="param_type"
	local DEVICE="$1"

	query_setup_data "$CONFIG_TEXT" "$TITLE" \
				"$QUESTION" "$DEVICE"
}
