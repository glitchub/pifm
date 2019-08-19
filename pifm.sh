#!/bin/bash -eu

die() { echo "$*" >&2; exit 1; }

# ensure kill children on exit
trap 'kill $(jobs -p) &>/dev/null' EXIT

usage() { die "Usage:

    pifm.sh [options]

Generate an FM test signal with Raspberry Pi 3.

Options are:

    -f MHz  - FM transmit frequency in MHz, default is 99.9
    -d KHz  - Maximum FM deviation in KHz, default is 75.0
    -s secs - Maximum time to transmit, in seconds, default is 0 == no timeout
    -t X    - Audio sine modulation frequency in Hz, default is 1000.0
    -a file - Play audio file instead of test tone (-t is ignored)

This script is a wrapper for the pifm executable, which must be in the same
directory." 
}

pifm=${0%/*}/pifm
[ -x $pifm ] || die "Need executable $pifm"

sox=$(type -P sox) || die "Need executable sox"

freq=99.9
tone=1000.0
deviation=""
timeout=""
file=""

while getopts ":f:t:d:s:a:" o; do case "$o" in
    f) freq=$OPTARG;;
    t) tone=$OPTARG;;
    d) deviation=$OPTARG;;
    s) timeout=$OPTARG;;
    a) file=$OPTARG; [[ -f $file ]] || die "No such file $file";;
    *) usage;;
esac; done

[[ $file ]] && what=$file || what="$tone Hz"
echo "Transmitting $what on FM $freq MHz${deviation:+ with $deviation KHz deviation}${timeout:+ for max $timeout seconds}"

# the sox output format expected by pifm
format="-r20000 -c1 -b16 -esigned -L -traw -"

# use sudo if not root
((UID)) && pifm="sudo -n $pifm"

if [[ $file ]]; then
    $sox $file $format
else
    $sox -n $format synth 0 sine $tone
fi | $pifm ${deviation:+-d$deviation} ${timeout:+-t$timeout} $freq   
