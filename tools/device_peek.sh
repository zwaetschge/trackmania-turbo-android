#!/bin/sh
# Read live guest memory of the running game on the device (debuggable APK, run-as).
# usage: device_peek.sh <hex guest va> [len=64] [user dir name=user]
cd "$(dirname "$0")/.."
VA=$1; LEN=${2:-64}; U=${3:-${TM_USER:-user}}
BASE=$(tools/device.sh sh "grep -m1 'guest space reserved' /sdcard/Android/data/ch.plum.tmturbo.recomp/files/$U/tmrecomp.log" | sed 's/.*host \(0x[0-9a-f]*\).*/\1/')
A=$(python3 -c "print($BASE + 0x$VA)")
tools/device.sh sh "P=\$(pidof ch.plum.tmturbo.recomp); run-as ch.plum.tmturbo.recomp xxd -s $A -l $LEN -g 4 /proc/\$P/mem" | python3 -c "
import sys
base=$BASE
for l in sys.stdin:
    p=l.split(':',1)
    if len(p)<2: print(l,end=''); continue
    a=int(p[0],16)-base; ws=p[1].split('  ')[0].split()
    print('%08X: '%a+' '.join(''.join(reversed([w[i:i+2] for i in range(0,len(w),2)])) for w in ws))"
