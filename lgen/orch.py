#!/usr/bin/python

# Orchestrator script to automate a lot of evaluation runs.
# This script must likely be adapted to your evaluation environment.

# Required: Python's pexpect (via apt-get or pip, ignore the syntax error in pip)

import argparse
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


SW_HOST="172.16.3.21"
CTRL_HOST="127.0.0.1"
LGEN_1_HOST="172.16.20.10"
LGEN_2_HOST="172.16.20.11"
LGENS_USERNAME="root"
LGENS_PW="omitted"			#we use a pubkey..."
SW_HOST_USERNAME="evaluator"
SW_HOST_PW="omitted"			#we use a pubkey..."

HANDOVER = True

LGENS_BINDIR="/home/lnobach/slim/code/handover_nf/target" if HANDOVER else "/home/lnobach/slim/code/nat_nf/target"
SW_HOST_EVALCMD="python /home/evaluator/eval-hv-load.py"
SW_HOST_POLLINTERVAL = 100000

OUTDIR = "/storage/slim-data"

BW_MULT = 1000000
NSEC_TO_SEC = 1000000



def getConnSendIntervalFromParams(bw, num_connections, packetsz):

  return int(1.0/bw*NSEC_TO_SEC*num_connections*packetsz*8)


class RemoteVNF():
	

	def __init__(self, vnfname, hostname, caller, extrasnapsize, legacy):
		self.hostname = hostname
		self.caller = caller
		self.extrasnapsize = extrasnapsize
		self.legacy = legacy
		self.logfilename = "run_" + vnfname + "_" + self.caller.runid + "_" + str(self.caller.repeatNo) + ".log"

	def onPrepare(self):
		try:                                                            
			self.s = pxssh.pxssh()
			hostname = self.hostname
			username = LGENS_USERNAME
			password = LGENS_PW
			self.s.login(hostname, username, password)
			self.s.sendline("(cd " + LGENS_BINDIR + "; ./run.sh " + str(self.extrasnapsize) + " " + str(self.legacy) + " | tee -i /tmp/" + self.logfilename + ")")
		except pxssh.ExceptionPxssh, e:
			print "pxssh failed on login."	
			raise

	def onCleanup(self):
		try:
			#self.s.sendcontrol("c")
			self.s.sendintr()
			self.s.prompt()
			self.s.sendline("cat /tmp/" + self.logfilename)
			self.s.prompt()
			outStr = self.s.before	#TODO: Strip first line!
			self.s.logout()

			outFName = self.caller.outputDirSmpl + "/" + self.logfilename
			outFile = open(outFName, "w")
			outFile.write(outStr)
			outFile.close()

		except pxssh.ExceptionPxssh, e:
			print "pxssh failed on login."	
			raise


class RemoteSwHost():
	
	# Poll interval in microsec, delay in msec
	def __init__(self, hostname, caller, pollinterval, interfacefilter):
		self.pollinterval = pollinterval
		self.hostname = hostname
		self.caller = caller
		self.tmpfile = "/tmp/sw_host_load_" + caller.runid + "_" + str(caller.repeatNo) + ".json"
		self.fstring = ",".join(interfacefilter)

	def onPrepare(self):
		try:                                                            
			self.s = pxssh.pxssh()
			hostname = self.hostname
			username = SW_HOST_USERNAME
			password = SW_HOST_PW
			self.s.login(hostname, username, password)
#			if self.delayms > 0:
#				print "Setting delay to " + str(self.delayms) + " milliseconds."
#				self.s.sendline(SW_HOST_SETDELAY + " " + str(self.delayms))
#				self.s.prompt()
#			else:
#				print "Deling delay."
#				self.s.sendline(SW_HOST_DELDELAY)
#				self.s.prompt()


		except pxssh.ExceptionPxssh, e:
			print "pxssh failed on login."	
			raise

	def onStart(self):
		self.timeStart = time.time()
		print "Starting load measurement..."
		try:         
			self.s.sendline(SW_HOST_EVALCMD + " " + self.tmpfile + " " + str(self.pollinterval) + " " + self.fstring)
		except pxssh.ExceptionPxssh, e:
			print "pxssh failed on onStart()"	
			raise

	def onCleanup(self):
		print "Stopping load measurement, ran " + str(time.time()-self.timeStart) + " seconds."
		try:
			
			#self.s.sendcontrol("c")
			self.s.sendintr()
			self.s.prompt()
			print "Load meter returned '" + self.s.before + "'"
			self.s.sendline("cat " + self.tmpfile)
			self.s.prompt()
			outStr = self.s.before	#TODO: Strip first line!
			print "Received " + str(len(outStr)) + " bytes of load sample data."
			self.s.logout()

			outFName = self.caller.outputDirSmpl + "/sw_host_load_" + str(self.caller.repeatNo) + ".json"
			outFile = open(outFName, "w")
			outFile.write(outStr)
			outFile.close()


		except pxssh.ExceptionPxssh, e:
			print "pxssh failed on login."	
			raise


class RemoteCtrl():
	

	def __init__(self, hostname, caller):
		self.hostname = hostname
		self.caller = caller

	def onPrepare(self):
		self.sendRESTCommand("i+vm0")

	def onMigrate(self):
		self.sendRESTCommand("m+vm0+vm1")

	def onCleanup(self):
		self.sendRESTCommand("p+vm0")
		self.sendRESTCommand("p+vm1")

	def sendRESTCommand(self, cmd):
		#curl -X POST 'http://localhost:8081/cmd?cmd=i+vm0'
		r = requests.post("http://" + self.hostname + ":8081/cmd?cmd=" + cmd)
		responseStr = r.text.rstrip('\n')
		print "Controller request for '" + cmd + "' returned '" + responseStr + "'"



class LocalLoadgen():

	def __init__(self, caller, bw, packetsz):
		self.caller = caller
		self.num_connections = 256 if HANDOVER else 128
		self.packetsz = packetsz
		self.runtime = 15
		
		self.conn_send_interval = getConnSendIntervalFromParams(bw, self.num_connections, self.packetsz)
		
		effectiveBW = 1.0/self.conn_send_interval*1000000*self.num_connections*self.packetsz*8
		print "Using effective bandwidth " + str(effectiveBW) + " bit/s"


	def onPrepare(self):
		print "Starting loadgen (output dir " + self.caller.outputDirRaw + ")..."
		if HANDOVER:
		  self.p = pexpect.spawn('./grt-lgen-handover', args=[self.caller.outputDirRaw, str(self.num_connections), str(self.packetsz), str(self.conn_send_interval), str(self.runtime), str(2000)])
		else:
		  self.p = pexpect.spawn('./grt-lgen', args=[self.caller.outputDirRaw, str(self.num_connections), str(self.packetsz), str(self.conn_send_interval), str(self.runtime)])
		print "Loadgen started."
		self.thr = Thread(target=self.handleLoadgenOut, args=())
		self.thr.start()
		

	def onEnd(self):
		self.p.terminate()

	def onCleanup(self):
		eval_rtt_ploss.RttPlossEval(self.caller.outputDirRaw, self.caller.outputDirSmpl + "/performance_" + str(self.caller.repeatNo) + ".json")

	def handleLoadgenOut(self):
		#Usage: ./grt-lgen <num_connections> <packetsz(bytes)> <conn_send_intvl_mean(msec)> <runtime(seconds)>
		
		try:
			while (True):
				self.p.expect('\n')
				print "LOADGEN: " + self.p.before
		except:
			print "Exception during loadgen listening..."
		

		print "LOADGEN finished."

class OrchestratedRun():

	def makedirsIfNotThere(self, d):
		try:
			os.makedirs(d)
		except OSError, e:
			print "Exception when trying to make output dir " + d + ", maybe existing. OK. ", e.message

	def __init__(self, runid, repeatNo, bw, pktSz, extrasnapshotsize=0, legacymode=0):

		print "Starting run with: " + str(runid) + ", " + str(repeatNo) + ", " + str(bw) + ", " + str(extrasnapshotsize) + ", " + str(legacymode) 

		self.repeatNo = repeatNo
		self.runid = runid
		self.outputDirRaw = OUTDIR + "/" + runname + "/raw/" + runid + "/" + str(repeatNo)
		self.outputDirSmpl = OUTDIR + "/" + runname + "/smpl/" + runid
		self.makedirsIfNotThere(self.outputDirRaw)
		self.makedirsIfNotThere(self.outputDirSmpl)

		self.ctrl = RemoteCtrl(CTRL_HOST, self)
		self.vnf1 = RemoteVNF("vm0", LGEN_1_HOST, self, extrasnapshotsize, legacymode)
		self.vnf2 = RemoteVNF("vm1", LGEN_2_HOST, self, extrasnapshotsize, legacymode)

		# Netronome: acquire with: npu//deps/sdk/p4/bin/rtecli tables -i 1 list-rules | awk '{print NR-1 "\t" $0}' | cut -c 1-60
		self.swhost = RemoteSwHost(SW_HOST, self, SW_HOST_POLLINTERVAL, ["13", "14", "15", "16", "17", "18", "19", "20", "21", "22", "23", "24"])
		self.lgen = LocalLoadgen(self, bw, pktSz)


	def onPrepare(self):
		self.vnf1.onPrepare()
		self.vnf2.onPrepare()
		#VNFs need at least 3 seconds to prepare themselves.
		time.sleep(3)
		self.ctrl.onPrepare()
		self.swhost.onPrepare()
		time.sleep(0.5) #Some time until traffic starts...
		self.lgen.onPrepare()
		


	def onStart(self):
		self.swhost.onStart()

	def onMigrate(self):
		self.ctrl.onMigrate()

	def onEnd(self):
		pass


	def onCleanup(self):
		self.ctrl.onCleanup()
		self.vnf1.onCleanup()
		self.vnf2.onCleanup()
		self.swhost.onCleanup()
		self.lgen.onCleanup()

	def run(self):

		print "Preparing..."
		self.onPrepare()
		time.sleep(2)
		print "Starting..."
		self.onStart()
		time.sleep(5)
		print "Migrating..."
		self.onMigrate()
		time.sleep(10)
		print "Ending..."
		self.onEnd()
		time.sleep(2)
		print "Cleaning up..."
		self.onCleanup()


def doOrchRunRepeat(runid, repeat, bw, packetSz, xSnapSz=0, legacymode=0):
	p = pexpect.spawn('killall ssh')
	time.sleep(1)
	print "Start run " + str(repeat)
	main = OrchestratedRun(runid, repeat, bw, packetSz, xSnapSz, legacymode)
	main.run()
	print "End run " + str(repeat)
	time.sleep(1)

def doOrchRunAllBW():
	for i in range(1,20):
		for bw in [50,100,150,200,250,300,350,400,450]:

			doOrchRunRepeat("default_bw" + str(bw) + "_pkt1400_dl05", i, bw*BW_MULT, 1400, xSnapSz=5120, legacymode=0)
			doOrchRunRepeat("legacy_bw" + str(bw) + "_pkt1400_dl05", i, bw*BW_MULT, 1400, xSnapSz=5120, legacymode=1)

			doOrchRunRepeat("default_bw" + str(bw) + "_pkt512_dl05", i, bw*BW_MULT, 512, xSnapSz=5120, legacymode=0)
			doOrchRunRepeat("legacy_bw" + str(bw) + "_pkt512_dl05", i, bw*BW_MULT, 512, xSnapSz=5120, legacymode=1)

#			doOrchRunRepeat("default_bw" + str(bw) + "_dl05_nosnapsz", bw, delayms=5, xSnapSz=0, legacymode=0)
#			doOrchRunRepeat("legacy_bw" + str(bw) + "_dl05_nosnapsz", bw, delayms=5, xSnapSz=0, legacymode=1)
		


parser = argparse.ArgumentParser(description='SliM Evaluation Orchestrator.')
parser.add_argument("runname", help="Name of run")
args = parser.parse_args()
runname = args.runname

doOrchRunAllBW()











