#!/bin/bash

SWITCH_IP=172.16.3.9
USER=lnobach
PROJNAME=slim-ofdpa
base=.

rsync -avzu  --exclude '*~' --exclude '.git*' $base $USER@$SWITCH_IP:/home/$USER/$PROJNAME
