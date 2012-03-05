# Copyright (C) 2012 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

. lib/test

name_prefix=$RANDOM

CHARACTER_WHITELIST="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789#+-.:=@_"
FAIL_MIXED_STR="contains mixed mangled and unmangled characters"
FAIL_MULTI_STR="seems to be multiple mangled"
FAIL_BLACK_STR="should be mangled but it contains blacklisted characters"
CORRECT_FORM_STR="name already in correct form"
RENAMING_STR="renaming to"

function create_dm_dev()
{
	local mode=$1
	local name=$2;

	if [ $mode = "none" ]; then
		# there's no mangling done - we must use --verifyudev here in
		# case we're testing with udev so we have the nodes in place,
		# udev would not create them - it can't handle unmangled names
		verify_udev="--verifyudev"
	else
		verify_udev=""
	fi

	dmsetup create "${name_prefix}$name" $verify_udev --manglename $mode --table "0 1 zero"
}

function remove_dm_dev()
{
	local mode=$1
	local name=$2

	if [ $mode = "none" ]; then
		verify_udev="--verifyudev"
	else
		verify_udev=""
	fi

	dmsetup remove $verify_udev --manglename $mode "${name_prefix}$name"
}

function check_create_and_remove()
{
	local mode=$1
	local input_name=$2
	local dm_name=$3
	local r=0

	if [ $mode = "none" ]; then
		verify_udev="--verifyudev"
	else
		verify_udev=""
	fi

	dmsetup create "${name_prefix}$input_name" $verify_udev --manglename $mode --table "0 1 zero" 2>err && \
	test -b "$DM_DEV_DIR/mapper/${name_prefix}$dm_name" && \
	dmsetup remove "${name_prefix}$input_name" $verify_udev --manglename $mode || r=1

	if [ $dm_name = "FAIL_MIXED" ]; then
		r=0
		grep "$FAILED_MIXED_STR" err || r=1
	elif [ $dm_name = "FAIL_MULTI" ]; then
		r=0
		grep "$FAILED_MULTI_STR" err || r=1
	elif [ $dm_name = "FAIL_BLACK" ]; then
		r=0
		grep "$FAILED_BLACK_STR" err || r=1
	fi

	return $r
}

function check_dm_field()
{
	local mode=$1
	local dm_name="$2"
	local field=$3
	local expected="$4"

	value=$(dmsetup info --rows --noheadings --manglename $mode -c -o $field "${DM_DEV_DIR}/mapper/${name_prefix}$dm_name" 2> err || true)

	if [ "$expected" = "FAIL_MIXED" ]; then
		grep "$FAIL_MIXED_STR" err
	elif [ "$expected" = "FAIL_MULTI" ]; then
		grep "$FAIL_MULTI_STR" err
	elif [ "$expected" = "FAIL_BLACK" ]; then
		grep "$FAIL_BLACK_STR" err
	else
		test "$value" = "${name_prefix}$expected"
	fi
}

function check_expected_names()
{
	local mode=$1
	local dm_name="$2"
	local r=0

	create_dm_dev none "$dm_name"

	test -b "$DM_DEV_DIR/mapper/${name_prefix}$dm_name" && \
	check_dm_field none "$dm_name" name "$dm_name" && \
	check_dm_field $mode "$dm_name" name "$3" && \
	check_dm_field $mode "$dm_name" mangled_name "$4" && \
	check_dm_field $mode "$dm_name" unmangled_name "$5" || r=1

	remove_dm_dev none "$dm_name"

	return $r
}

function check_mangle_cmd()
{
	local mode=$1
	local dm_name="$2"
	local expected="$3"
	local rename_expected=0
	local r=0

	create_dm_dev none "$dm_name"

	dmsetup mangle --manglename $mode "${name_prefix}$dm_name" 1>out 2>err || true;

	if [ "$expected" = "OK" ]; then
		grep "$CORRECT_FORM_STR" out || r=1
	elif [ "$expected" = "FAIL_MIXED" ]; then
		grep "$FAIL_MIXED_STR" err || r=1
	elif [ "$expected" = "FAIL_MULTI" ]; then
		grep "$FAIL_MULTI_STR" err || r=1
	else
		rename_expected=1
		grep -F "$RENAMING_STR ${name_prefix}$expected" out || r=1
	fi

	if [ $r = 0 -a $rename_expected = 1 ]; then
		# successfuly renamed to expected name
		remove_dm_dev none "$expected"
	elif [ $r = 1 ]; then
		# failed to rename to expected or renamed when it should not - find the new name
		new_name=$(sed -e "s/.*: $RENAMING_STR //g" out)
		# try to remove any of the form - falling back to less probable error scenario
		dmsetup remove --verifyudev --manglename none "$new_name" || \
		remove_dm_dev none "$dm_name" || remove_dm_dev none "$expected"
	else
		# successfuly done nothing
		remove_dm_dev none "$dm_name"
	fi

	return $r
}

# check dmsetup can process path where the last component is not equal dm name (rhbz #797322)
r=0
create_dm_dev auto "abc"
ln -s ${DM_DEV_DIR}/mapper/${name_prefix}abc ${DM_DEV_DIR}/${name_prefix}xyz
dmsetup status ${DM_DEV_DIR}/${name_prefix}xyz || r=1
remove_dm_dev auto "abc"
if [ r = 1 ]; then
	exit 1
fi

### ALL WHITELISTED CHARACTERS ###
# none of these should be mangled in any mode
name="$CHARACTER_WHITELIST"
for mode in auto hex none; do
	check_expected_names $mode "$name" "$name" "$name" "$name"
	check_mangle_cmd $mode "$name" "OK"
done


#### NONE MANGLING MODE ###
check_create_and_remove none 'a b' 'a b'
check_create_and_remove none 'a\x20b' 'a\x20b'
check_create_and_remove none 'a b\x20c' 'a b\x20c'
check_create_and_remove none 'a\x5cx20b' 'a\x5cx20b'

check_expected_names none 'a b' 'a b' 'a\x20b' 'a b'
check_expected_names none 'a\x20b' 'a\x20b' 'a\x20b' 'a b'
check_expected_names none 'a b\x20c' 'a b\x20c' 'FAIL_MIXED' 'a b c'
check_expected_names none 'a\x5cx20b' 'a\x5cx20b' 'a\x5cx20b' 'a\x20b'

check_mangle_cmd none 'a b' 'OK'
check_mangle_cmd none 'a\x20b' 'a b'
check_mangle_cmd none 'a b\x20c' 'a b c'
check_mangle_cmd none 'a\x5cx20b' 'a\x20b'


### AUTO MANGLING MODE ###
check_create_and_remove auto 'a b' 'a\x20b'
check_create_and_remove auto 'a\x20b' 'a\x20b'
check_create_and_remove auto 'a b\x20c' 'FAIL_MIXED'
check_create_and_remove auto 'a\x5cx20b' 'FAIL_MULTI'

check_expected_names auto 'a b' 'FAIL_BLACK' 'FAIL_BLACK' 'FAIL_BLACK'
check_expected_names auto 'a\x20b' 'a b' 'a\x20b' 'a b'
check_expected_names auto 'a b\x20c' 'FAIL_BLACK' 'FAIL_BLACK' 'FAIL_BLACK'
check_expected_names auto 'a\x5cx20b' 'FAIL_MULTI' 'FAIL_MULTI' 'FAIL_MULTI'

check_mangle_cmd auto 'a b' 'a\x20b'
check_mangle_cmd auto 'a\x20b' 'OK'
check_mangle_cmd auto 'a b\x20c' 'FAIL_MIXED'
check_mangle_cmd auto 'a\x5cx20b' 'FAIL_MULTI'


### HEX MANGLING MODE ###
check_create_and_remove hex 'a b' 'a\x20b'
check_create_and_remove hex 'a\x20b' 'a\x5cx20b'
check_create_and_remove hex 'a b\x20c' 'a\x20b\x5cx20c'
check_create_and_remove hex 'a\x5cx20b' 'a\x5cx5cx20b'

check_expected_names hex 'a b' 'FAIL_BLACK' 'FAIL_BLACK' 'FAIL_BLACK'
check_expected_names hex 'a\x20b' 'a b' 'a\x20b' 'a b'
check_expected_names hex 'a b\x20c' 'FAIL_BLACK' 'FAIL_BLACK' 'FAIL_BLACK'
check_expected_names hex 'a\x5cx20b' 'a\x20b' 'a\x5cx20b' 'a\x20b'

check_mangle_cmd hex 'a b' 'a\x20b'
check_mangle_cmd hex 'a\x20b' 'OK'
check_mangle_cmd hex 'a b\x20c' 'FAIL_MIXED'
check_mangle_cmd hex 'a\x5cx20b' 'OK'
