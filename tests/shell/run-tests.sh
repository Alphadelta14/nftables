#!/bin/bash

msg_error() {
	echo "E: $1 ..." >&2
	exit 1
}

msg_warn() {
	echo "W: $1" >&2
}

msg_info() {
	echo "I: $1"
}

usage() {
	echo " $0 [OPTIONS]"
	echo
	echo "OPTIONS:"
	echo " -h|--help : Print usage."
	echo " -v        : Sets VERBOSE=y."
	echo " -g        : Sets DUMPGEN=y."
	echo " -V        : Sets VALGRIND=y."
	echo " -K        : Sets KMEMLEAK=y."
	echo
	echo "ENVIRONMENT VARIABLES:"
	echo " NFT=<CMD>    : Path to nft executable. Will be called as \`\$NFT [...]\` so"
	echo "                it can be a command with parameters. Note that in this mode quoting"
	echo "                does not work, so the usage is limited and the command cannot contain"
	echo "                spaces."
	echo " VERBOSE=*|y  : Enable verbose output."
	echo " DUMPGEN=*|y  : Regenerate dump files."
	echo " VALGRIND=*|y : Run \$NFT in valgrind."
	echo " KMEMLEAK=*|y : Check for kernel memleaks."
}

# Configuration
TESTDIR="./$(dirname $0)/testcases"
SRC_NFT="$(dirname $0)/../../src/nft"
DIFF=$(which diff)

if [ "$(id -u)" != "0" ] ; then
	msg_error "this requires root!"
fi

if [ "${1}" != "run" ]; then
	if unshare -f -n true; then
		unshare -n "${0}" run $@
		exit $?
	fi
	msg_warn "cannot run in own namespace, connectivity might break"
fi
shift

VERBOSE="$VERBOSE"
DUMPGEN="$DUMPGEN"
VALGRIND="$VALGRIND"
KMEMLEAK="$KMEMLEAK"

TESTS=()

while [ $# -gt 0 ] ; do
	A="$1"
	shift
	case "$A" in
		-v)
			VERBOSE=y
			;;
		-g)
			DUMPGEN=y
			;;
		-V)
			VALGRIND=y
			;;
		-K)
			KMEMLEAK=y
			;;
		-h|--help)
			usage
			exit 0
			;;
		--)
			TESTS+=( "$@" )
			shift $#
			;;
		*)
			# Any unrecognized option is treated as a test name, and also
			# enable verbose tests.
			TESTS+=( "$A" )
			VERBOSE=y
			;;
	esac
done

SINGLE="${TESTS[*]}"

[ -z "$NFT" ] && NFT=$SRC_NFT
${NFT} > /dev/null 2>&1
ret=$?
if [ ${ret} -eq 126 ] || [ ${ret} -eq 127 ]; then
	msg_error "cannot execute nft command: ${NFT}"
else
	msg_info "using nft command: ${NFT}"
fi

if [ ! -d "$TESTDIR" ] ; then
	msg_error "missing testdir $TESTDIR"
fi

FIND="$(which find)"
if [ ! -x "$FIND" ] ; then
	msg_error "no find binary found"
fi

MODPROBE="$(which modprobe)"
if [ ! -x "$MODPROBE" ] ; then
	msg_error "no modprobe binary found"
fi

DIFF="$(which diff)"
if [ ! -x "$DIFF" ] ; then
	DIFF=true
fi

kernel_cleanup() {
	$NFT flush ruleset
	$MODPROBE -raq \
	nft_reject_ipv4 nft_reject_bridge nft_reject_ipv6 nft_reject \
	nft_redir_ipv4 nft_redir_ipv6 nft_redir \
	nft_dup_ipv4 nft_dup_ipv6 nft_dup nft_nat \
	nft_masq_ipv4 nft_masq_ipv6 nft_masq \
	nft_exthdr nft_payload nft_cmp nft_range \
	nft_quota nft_queue nft_numgen nft_osf nft_socket nft_tproxy \
	nft_meta nft_meta_bridge nft_counter nft_log nft_limit \
	nft_fib nft_fib_ipv4 nft_fib_ipv6 nft_fib_inet \
	nft_hash nft_ct nft_compat nft_rt nft_objref \
	nft_set_hash nft_set_rbtree nft_set_bitmap \
	nft_synproxy nft_connlimit \
	nft_chain_nat \
	nft_chain_route_ipv4 nft_chain_route_ipv6 \
	nft_dup_netdev nft_fwd_netdev \
	nft_reject nft_reject_inet nft_reject_netdev \
	nf_tables_set nf_tables \
	nf_flow_table nf_flow_table_ipv4 nf_flow_tables_ipv6 \
	nf_flow_table_inet nft_flow_offload \
	nft_xfrm
}

find_tests() {
	if [ ! -z "$SINGLE" ] ; then
		echo $SINGLE
		return
	fi
	${FIND} ${TESTDIR} -type f -executable | sort
}

printscript() { # (cmd, tmpd)
	cat <<EOF
#!/bin/bash

CMD="$1"

# note: valgrind man page warns about --log-file with --trace-children, the
# last child executed overwrites previous reports unless %p or %q is used.
# Since libtool wrapper calls exec but none of the iptables tools do, this is
# perfect for us as it effectively hides bash-related errors

valgrind --log-file=$2/valgrind.log --trace-children=yes \
	 --leak-check=full --show-leak-kinds=all \$CMD "\$@"
RC=\$?

# don't keep uninteresting logs
if grep -q 'no leaks are possible' $2/valgrind.log; then
	rm $2/valgrind.log
else
	mv $2/valgrind.log $2/valgrind_\$\$.log
fi

# drop logs for failing commands for now
[ \$RC -eq 0 ] || rm $2/valgrind_\$\$.log

exit \$RC
EOF
}

if [ "$VALGRIND" == "y" ]; then
	tmpd=$(mktemp -d)
	chmod 755 $tmpd

	msg_info "writing valgrind logs to $tmpd"

	printscript "$NFT" "$tmpd" >${tmpd}/nft
	trap "rm ${tmpd}/nft" EXIT
	chmod a+x ${tmpd}/nft

	NFT="${tmpd}/nft"
fi

echo ""
ok=0
failed=0
taint=0

check_taint()
{
	read taint_now < /proc/sys/kernel/tainted
	if [ $taint -ne $taint_now ] ; then
		msg_warn "[FAILED]	kernel is tainted: $taint  -> $taint_now"
	fi
}

kmem_runs=0
kmemleak_found=0

check_kmemleak_force()
{
	test -f /sys/kernel/debug/kmemleak || return 0

	echo scan > /sys/kernel/debug/kmemleak

	lines=$(grep "unreferenced object" /sys/kernel/debug/kmemleak | wc -l)
	if [ $lines -ne $kmemleak_found ];then
		msg_warn "[FAILED]	kmemleak detected $lines memory leaks"
		kmemleak_found=$lines
	fi

	if [ $lines -ne 0 ];then
		return 1
	fi

	return 0
}

check_kmemleak()
{
	test -f /sys/kernel/debug/kmemleak || return

	if [ "$KMEMLEAK" == "y" ] ; then
		check_kmemleak_force
		return
	fi

	kmem_runs=$((kmem_runs + 1))
	if [ $((kmem_runs % 30)) -eq 0 ]; then
		# scan slows tests down quite a bit, hence
		# do this only for every 30th test file by
		# default.
		check_kmemleak_force
	fi
}

check_taint

for testfile in $(find_tests)
do
	read taint < /proc/sys/kernel/tainted
	kernel_cleanup

	msg_info "[EXECUTING]	$testfile"
	test_output=$(NFT="$NFT" DIFF=$DIFF ${testfile} 2>&1)
	rc_got=$?
	echo -en "\033[1A\033[K" # clean the [EXECUTING] foobar line

	if [ "$rc_got" -eq 0 ] ; then
		# check nft dump only for positive tests
		dumppath="$(dirname ${testfile})/dumps"
		dumpfile="${dumppath}/$(basename ${testfile}).nft"
		rc_spec=0
		if [ "$rc_got" -eq 0 ] && [ -f ${dumpfile} ]; then
			test_output=$(${DIFF} -u ${dumpfile} <($NFT list ruleset) 2>&1)
			rc_spec=$?
		fi

		if [ "$rc_spec" -eq 0 ]; then
			msg_info "[OK]		$testfile"
			[ "$VERBOSE" == "y" ] && [ ! -z "$test_output" ] && echo "$test_output"
			((ok++))

			if [ "$DUMPGEN" == "y" ] && [ "$rc_got" == 0 ] && [ ! -f "${dumpfile}" ]; then
				mkdir -p "${dumppath}"
				$NFT list ruleset > "${dumpfile}"
			fi
		else
			((failed++))
			if [ "$VERBOSE" == "y" ] ; then
				msg_warn "[DUMP FAIL]	$testfile: dump diff detected"
				[ ! -z "$test_output" ] && echo "$test_output"
			else
				msg_warn "[DUMP FAIL]	$testfile"
			fi
		fi
	else
		((failed++))
		if [ "$VERBOSE" == "y" ] ; then
			msg_warn "[FAILED]	$testfile: got $rc_got"
			[ ! -z "$test_output" ] && echo "$test_output"
		else
			msg_warn "[FAILED]	$testfile"
		fi
	fi

	check_taint
	check_kmemleak
done

echo ""

# kmemleak may report suspected leaks
# that get free'd after all, so always do
# a check after all test cases
# have completed and reset the counter
# so another warning gets emitted.
kmemleak_found=0
check_kmemleak_force

msg_info "results: [OK] $ok [FAILED] $failed [TOTAL] $((ok+failed))"

kernel_cleanup
[ "$failed" -eq 0 ]
