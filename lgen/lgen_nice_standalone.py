#!/usr/bin/python

# Wrapper script to run the lgen(s) with nicer command-line parameters.

import time
from pexpect import pxssh
import pexpect
import requests
import os
import sys
import subprocess
from threading import Thread
ON_POSIX = 'posix' in sys.builtin_module_names
import eval_rtt_ploss
import argparse

NSEC_TO_SEC = 1000000

HANDOVER = False #Set to true if you want to run the lgen_handover instead of the default one.


def getConnSendIntervalFromParams(bw, num_connections, packetsz):
  return int(1.0/bw*NSEC_TO_SEC*num_connections*packetsz*8)
    

class LocalLoadgen():

	def __init__(self, num_connections, packetsz, runtime, bw):

		self.outputDirRaw = "test/raw"
		self.outputDirSmpl = "test/smpl"

		try:
			os.makedirs(self.outputDirRaw)
		except OSError, e:
			print "Exception when trying to make output dirs, maybe existing. OK. ", e.message

		try:
			os.mkdir(self.outputDirSmpl)
		except OSError, e:
			print "Exception when trying to make output dirs, maybe existing. OK. ", e.message


		self.num_connections = num_connections
		self.packetsz = packetsz
		self.runtime = runtime

		self.connSendInterval = getConnSendIntervalFromParams(bw, self.num_connections, self.packetsz)

		print "connSendInterval is " + str(self.connSendInterval)
		effectiveBW = 1.0/self.connSendInterval*NSEC_TO_SEC*self.num_connections*self.packetsz*8
		print "Using effective bandwidth " + str(effectiveBW) + " bit/s (doubles on the way back!)"


	def start(self):
		print "Starting loadgen (output dir " + self.outputDirRaw + ")..."
		if HANDOVER:
		  self.p = pexpect.spawn('./grt-lgen-handover', args=[self.outputDirRaw, str(self.num_connections), str(self.packetsz), str(self.connSendInterval), str(self.runtime), str(2000)])
		else:
		  self.p = pexpect.spawn('./grt-lgen', args=[self.outputDirRaw, str(self.num_connections), str(self.packetsz), str(self.connSendInterval), str(self.runtime)])
		print "Loadgen started..."
		#Usage: ./grt-lgen <num_connections> <packetsz(bytes)> <conn_send_intvl_mean(msec)> <runtime(seconds)>
		
		try:
			while (True):
				self.p.expect('\n')
				print "LOADGEN: " + self.p.before
		except:
			print "Loadgen closed, either normally or due to an error..."
		

		print "LOADGEN finished."		

	def processData(self):
		eval_rtt_ploss.RttPlossEval(self.outputDirRaw, self.outputDirSmpl + "/performance.json", csv=True)






parser = argparse.ArgumentParser(description='Load generator nice startup script.')
parser.add_argument("bandwidth", help="Bandwidth in MBit/s")
parser.add_argument("runtime", help="Duration to run in seconds")
parser.add_argument("pktsz", help="Packet size in bytes")
args = parser.parse_args()
bandwidth = float(args.bandwidth)

num_connections = 256 if HANDOVER else 128
packetsz = int(args.pktsz)
runtime = int(args.runtime)

lgen = LocalLoadgen(num_connections, packetsz, runtime, bandwidth*1000000)
lgen.start()
lgen.processData()













