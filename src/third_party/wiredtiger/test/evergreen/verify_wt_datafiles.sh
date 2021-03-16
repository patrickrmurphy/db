#! /usr/bin/env bash
#
# This script is used to list and dump WiredTiger data files ('WiredTiger.wt') generated by test/format program.
#
# Supported return values:
#   0: list & verify & dump are successful for all data files
#   1: list is not successful for some data file
#   2: verify is not successful for some table object
#   3: dump is not successful for some table object
#   4: No table is dumped out from all data files
#   5: Command argument setting is wrong
#   6: 't' or 'wt' binary file does not exist
#   7: 'WT_TEST' directories do not exist

set -eu

verbose=false

if [ $# -gt 1 ]; then
	echo "Usage: $0 [-v]"
	exit 5
elif [ $# -eq 1 ]; then
	[ "$1" == "-v" ] && verbose=true || verbose=false
fi

# Switch to the Git repo toplevel directory
cd $(git rev-parse --show-toplevel)

# Walk into the test/format directory in which data files are generated
cd build_posix/test/format

# Check the existence of 'WT_TEST' directories
num_dirs=$(find . -type d -name 'WT_TEST.[0-9]*' | wc -l)
if [ "${num_dirs}" -eq "0" ]; then
	echo "test/format 'WT_TEST' directories do not exist, exiting ..."
	exit 7
fi

# Check the existence of 'wt' binary
wt_binary="../../.libs/wt"
if [ ! -x "${wt_binary}" ]; then
	echo "'wt' binary does not exist, exiting ..."
	exit 6
fi

export LD_LIBRARY_PATH=${LD_LIBRARY_PATH:-""}:$(dirname "${wt_binary}")
num_tables_verified=0

# Work out the list of directories that include wt data files
dirs_include_datafile=$(find ./WT_TEST.[0-9]* -type f -name WiredTiger.wt -print0 | xargs -0 dirname)
echo -e "\n[dirs_include_datafile]:\n${dirs_include_datafile}\n"

# Loop through each data file under the TEST_DIR
IFS=$'\n'
for d in ${dirs_include_datafile}
do
	echo "${d}"

	${wt_binary} -h ${d} printlog > /dev/null
	if [ "$?" -ne "0" ]; then
		echo "Failed to dump '${d}' log files, exiting ..."
		exit 1
	fi

	tables=$(${wt_binary} -h "${d}" list)
	if [ "$?" -ne "0" ]; then 
		echo "Failed to list '${d}' directory, exiting ..."
		exit 1
	fi

	# Loop through each table object in the data file
	for t in ${tables}
	do
		echo ${t}
		${wt_binary} -h ${d} verify ${t}
		if [ "$?" -ne "0" ]; then
			echo "Failed to verify '${t}' table under '${d}' directory, exiting ..."
			exit 2
		fi

		if [ "${verbose}" == true ]; then
			${wt_binary} -h ${d} dump ${t}
		else
			${wt_binary} -h ${d} dump ${t} > /dev/null
		fi

		if [ "$?" -ne "0" ]; then
			echo "Failed to dump '${t}' table under '${d}' directory, exiting ..."
			exit 3
		fi

		# Table verification is successful, increment the counter
		let "num_tables_verified+=1"
		echo "num_tables_verified = ${num_tables_verified}"
	done
done

if [ "${num_tables_verified}" -eq 0 ]; then
	echo "No table is verified from all data files, something could have gone wrong ..."
	exit 4
fi

# If reaching here, the testing result is positive
echo -e "\nAll data files under 'WT_TEST' directories are verified successfully!"
exit 0 
