#!/bin/sh
w=/srv/joist/test/timerout
cd $w
while ! grep -q "stress finished" stress.txt; do sleep 5; done
./campaign.sh 7 to-new-sc to-old-sc to-new-lb to-old-lb
echo "timing finished $(date +%T)" >> $w/campaign.log
