# Runs on the hypervisor running the SliM VM(s) and collects interface statistics. E.g. used by orch.py

import struct
from os import listdir
from os.path import isfile, join
import pprint
import math
import time
import json
import signal
import argparse
import subprocess
import sys

pp = pprint.PrettyPrinter(indent=4)
TIME_UNIT_FACTOR = 1000000

# For Netronome NFP NICs in the hypervisor.
P4_NETRONOME_RTECLI = "/opt/rtecli"
P4_NETRONOME_TABLE = "vlan_counter_b_bytes"
P4_NETRONOME_MAXVALS = 256

class LoadEvaluator():

	def __init__(self, interval, outfilename):
		self.outfilename = outfilename
		self.interval = interval
		self.stopped = False

	def startCollectStats(self, interfacefilter):
		fil3 = open(self.outfilename, "w")
		fil3.write("[")
		firstline = True
		try:
			while not self.stopped:
				timeBefore = time.time()
				currentTimeMicros = int(timeBefore*TIME_UNIT_FACTOR)
				currentStats = self.getCurrentStats_nfp(interfacefilter)	#SR-IOV or Linux? Add here.
				if not firstline:
					fil3.write(",")
				fil3.write(json.dumps([currentTimeMicros, currentStats]) + "\n")
				timeAfter = time.time()
				sleepTime = self.interval/TIME_UNIT_FACTOR - (timeAfter - timeBefore)
				#print "Sleeping for " + str(sleepTime) + "..."
				if (sleepTime > 0):
					time.sleep(sleepTime)
				firstline = False
		except:
			e = sys.exc_info()[0]
			print e
		fil3.write("]\n")
		fil3.close()


# Get the n

	def getCurrentStats_nfp(self, interfacefilter):

		p = subprocess.Popen([P4_NETRONOME_RTECLI, "counters", "-c", P4_NETRONOME_TABLE, "get"], 
			stdout=subprocess.PIPE, stderr=subprocess.PIPE)
		out, err = p.communicate()

		#print "out:'" + out + "'"
		#TODO: error handling?
		vals = out.split(",", P4_NETRONOME_MAXVALS)
		result = {}
		for i_s in interfacefilter:
			i = int(i_s)

			result[i_s] = {}

			if i == 0:
				result[i_s]["bytes"] = int(vals[i][1:])
			else:
				result[i_s]["bytes"] = int(vals[i])
		return result
		
		

	def getCurrentStats_sriov(self, interfacefilter):
		result = {}
		for intf in interfacefilter:

			value = {}
			intname, vf = intf.split(":", 1)

			f = open("/sys/class/net/" + intname + "/vf" + vf  + "/statistics/rx_bytes", "r")
			rxBytesStr = f.read()
			f.close()
			f = open("/sys/class/net/" + intname + "/vf" + vf  + "/statistics/rx_packets", "r")
			rxPktsStr = f.read()
			f.close()
			f = open("/sys/class/net/" + intname + "/vf" + vf  + "/statistics/tx_bytes", "r")
			txBytesStr = f.read()
			f.close()	
			f = open("/sys/class/net/" + intname + "/vf" + vf  + "/statistics/tx_packets", "r")
			txPktsStr = f.read()
			f.close()	

			value["rxBytes"] = int(rxBytesStr)
			value["rxPkts"] = int(rxPktsStr)

			value["txBytes"] = int(txBytesStr)
			value["txPkts"] = int(txPktsStr)
			result[intf] = value
		

		f.close()
		return result

	def getCurrentStats(self, interfacefilter):
		f = open("/proc/net/dev", "r")
		result = {}
		for line in f:
			#print "Line: " + line
			parts = line.split()
			key = parts[0].strip(':')
			
			if key in interfacefilter:
				value = {}
				value["rxBytes"] = int(parts[1])
				value["rxPkts"] = int(parts[2])

				value["txBytes"] = int(parts[9])
				value["txPkts"] = int(parts[10])
				result[key] = value
			


	def signalHandler(self, signum, frame):
		self.stopped = True


parser = argparse.ArgumentParser(description='Regularly probe interfaces for stats. Also prints the node time in microseconds')
parser.add_argument("filename", help="File to write to")
parser.add_argument("interval", help="Interval for probing the interface stats (in microseconds)")
parser.add_argument("interfacefilter", help="Interface filter to use, comma separated e.g. eth0,eth1")
args = parser.parse_args()
interval = float(args.interval)
intfilter = args.interfacefilter.split(",")

evalu = LoadEvaluator(interval, args.filename)
signal.signal(signal.SIGINT, evalu.signalHandler)
evalu.startCollectStats(intfilter)



























