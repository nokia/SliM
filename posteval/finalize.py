#!/usr/bin/python

# Creates gnuplottable CSV records from the results of orch.py

import os
import pprint
import json
import math

import numpy as np
import scipy as sp
import scipy.stats
import subprocess

pp = pprint.PrettyPrinter(indent=4)


#baseTimeOffset = 500000	#usec

intTimeUnit = 0.000001	#usec

TIME_QUANTIZATION = 500000
SECOND = 1000000

minSamplesConsider = 3 #Only consider elements for "peak over 500ms" where a minimum number of samples exist. Otherwise confidence could be insufficient.


class StreamStat:

	def getMeanCI(self, confidence=0.95):
		n = self.getNumSamples()
		stdev = self.getStdDev()
		ciDev = sp.stats.t._ppf((1+confidence)/2., n-1) * stdev / math.sqrt(n)

		return ciDev

	def __init__(self):
		self.aggr = 0
		self.m = 0.0
		self.s = 0.0
		self.k = 0

	def addValue(self, val):

		if math.isnan(val):
			print "Discovered NaN."
			return

		self.aggr += val

		#Using Wolford's method here: http://www.johndcook.com/blog/standard_deviation/
		last_m = self.m
		self.k = self.k + 1
		self.m += (val - last_m) / self.k
		self.s += (val - last_m) * (val - self.m)

	def getStdDev(self):
		if (self.k <= 1):
			return float('nan')
		return math.sqrt(self.s/(self.k-1))

	def getMean(self):
		if self.k <= 0:
			return float('nan')
		return self.aggr/self.k

	def getNumSamples(self):
		return self.k

	@classmethod
	def getCSVKey(cls, outFName):
		#return [outFName + "_mean", outFName + "_stdDev", outFName + "_numSamples"]
		return [outFName + "_mean", outFName + "_ciDiff", outFName + "_numSamples"]

	def getValsCSV(self):
		#return [str(self.getMean()), str(self.getStdDev()), str(self.getNumSamples())]
		return [str(self.getMean()), str(self.getMeanCI()), str(self.getNumSamples())]

	@classmethod
	def doTest(cls):
		statTest1 = StreamStat()
		test1 = [110, 112, 106, 90, 96, 118, 108, 114,107,90,85,84,113,105,90,104]
		for val in test1:
			statTest1.addValue(val)
		print statTest1.getStdDev()
		print statTest1.getValsCSV()




class TimeQuantizedElement():

	def __init__(self):
		self.mgtBytesVM0 = StreamStat()
		self.mgtBytesVM1 = StreamStat()
		self.dpBytesVM0 = StreamStat()
		self.dpBytesVM1 = StreamStat()

		self.perfRTT = StreamStat()
		self.lossRatio = StreamStat()

	def addPerf(self, val):		
		#pp.pprint(val)
		self.perfRTT.addValue(val['rtt_mean_nsec'])
		self.lossRatio.addValue(val['loss_ratio'])

	def addCost(self, val):
		self.mgtBytesVM0.addValue(val["13"]["bytes"] + val["14"]["bytes"])
		self.mgtBytesVM1.addValue(val["19"]["bytes"] + val["20"]["bytes"])

		dpBytesVM0 = val["15"]["bytes"] + val["17"]["bytes"]	
		#print "Adding value " + str(dpBytesVM0)	
		self.dpBytesVM0.addValue(dpBytesVM0)

		dpBytesVM1 = val["21"]["bytes"] + val["23"]["bytes"]		
		self.dpBytesVM1.addValue(dpBytesVM1)

	@classmethod
	def getCSVKey(cls):
		return StreamStat.getCSVKey("mgtBytesVM0") + \
			StreamStat.getCSVKey("mgtBytesVM1") + \
			StreamStat.getCSVKey("perfRTT") + \
			StreamStat.getCSVKey("lossRatio") + \
			StreamStat.getCSVKey("dpBytesVM0") + \
			StreamStat.getCSVKey("dpBytesVm1")

	def getValsCSV(self):
		return self.mgtBytesVM0.getValsCSV() + \
			self.mgtBytesVM1.getValsCSV() + \
			self.perfRTT.getValsCSV() + \
			self.lossRatio.getValsCSV() + \
			self.dpBytesVM0.getValsCSV() + \
			self.dpBytesVM1.getValsCSV()




class TimeQuantizer():

	def __init__(self, qInt):
		self.qInt = qInt
		self.m4p = {}


	def getNextSmallerEq(self, keylim):
		currentKey = None
		currentVal = None
		for key, value in self.m4p.iteritems():
			if key > currentKey and key <= keylim:
				currentKey = key
				currentVal = value
		return currentKey, currentVal

	def getQuant(self, val):
		return np.rint(float(val)/self.qInt)*self.qInt


	def addAllPerf(self, vals):
		for val in vals:
			self.addPerf(val) 

	def addAllCost(self, vals):
		for val in vals:
			self.addCost(val)

	def addPerf(self, val):
		quant = self.getQuant(val[0])
		if not quant in self.m4p:
			self.m4p[quant] = TimeQuantizedElement()
		self.m4p[quant].addPerf(val[1])

	def addCost(self, val):
		quant = self.getQuant(val[0])
		if not quant in self.m4p:
			self.m4p[quant] = TimeQuantizedElement()
		self.m4p[quant].addCost(val[1])
	
	@classmethod
	def getCSVKey(cls):
		return [ "Time", ] + TimeQuantizedElement.getCSVKey()

	def getLargestElement(self, functionToGetVal):
		highestVal = None
		for key, value in self.m4p.iteritems():			
			valCandidate = functionToGetVal(value)
			if math.isnan(valCandidate[0]):
				continue
#			print " Found element " + str(key) + ", " + str(valCandidate[1].getValsCSV())
			if highestVal == None or highestVal[0] < valCandidate[0]:
				highestVal = valCandidate
				highestKey = key
		if highestVal == None:
			return None, TimeQuantizedElement()
#		print "Highest val is " + str(highestVal[1].getValsCSV())
		return highestKey, self.m4p[highestKey]


	def writeToCSVFile(self, outFName):
		f = open(outFName, "w")

		i = 1	#Gnuplot starts with 1
		csvkeys = []
		for key in TimeQuantizer.getCSVKey():
			csvkeys.append(key + "(" + str(i) + ")")
			i += 1

		f.write("#" + "\t".join(csvkeys) + "\n")

		for key, value in sorted(self.m4p.iteritems()):
			f.write("\t".join([str(key*intTimeUnit) ,] + value.getValsCSV()) + "\n")

		f.close()



class SLAQuota():

	def __init__(self, srcVal):

		self.srcVal = srcVal	#This denotes whether this object is for a specific SLA level or the overall packet return.

		self.absoluteVals = StreamStat()
		self.relativeVals = StreamStat()

	def addRun(self, run):
		#print run

		remove_time = 2 * 1000000	#2s seconds removed (as lgen might not be ready and shows packetloss.

		totalTime = 0
		sentPackets = 0
		rcvdPackets = 0
		lastTime = -1

		for [time, vals] in run:
			#print time
			if time < remove_time:
				lastTime = time
				continue;
			sentPackets += vals["total_packets"]
			rcvdPackets += vals[self.srcVal]
			if lastTime < 0:
				print "Exception: Last time not set!"
				exit()
			totalTime += time - lastTime
			lastTime = time

		lostPackets = sentPackets - rcvdPackets
		pps = sentPackets / (totalTime/SECOND)
		lostQuota = float(lostPackets)/float(pps)	#The metric is: total packets lost divided by PPS rate

		self.absoluteVals.addValue(lostPackets)
		self.relativeVals.addValue(lostQuota)
		
		print "srcVal=" + self.srcVal + ", PPS: " + str(pps) + ", lostPackets = " + str(lostPackets) + ", lostQuota = " + str(lostQuota)



class FinalizerMain():


	def getFromJSON(self, infile):
		#print "Opening file " + infile
		f = open(infile, "r")
		s = f.read()	#TODO: could be large!!!
		f.close()
		if not s.startswith("["):
			s = s.split("\n", 1)[1]
			#print(str(len(s[0])) + ", " + str(len(s[1])))
		#print "'" + s + "'"

		if not s[-20:].replace("\n","").replace("\r","").replace(" ","").endswith("]]"):	# Quirks mode if crashed before last bracket.
			s += "]"

		#print s

		#print "<" + s[-20:] + ">"

		return json.loads(s)

	def normalizeCostVal(self, costVal, firstCostVal):
		res = {}
		for key,value in costVal.iteritems():
			try:
				valueResult = {}
				for key2 in value.keys():
					valueResult[key2] = value[key2] - firstCostVal[key][key2]
				res[key] = valueResult
			except:
				res[key] = value - firstCostVal[key]
			
		return res



	def getRunNormalized(self, costRun, perfRun):
		
		#From now, we do everything in microseconds. We have to convert from the performance input (nsec).

		baseTime = costRun[0][0]

		perfStartTime = perfRun[0][0]/1000

		print "Base time (cost start) is " + str(baseTime) + ", perf measurement started at " + str(perfStartTime)  + " i.e. " +  str(baseTime - perfStartTime) + " usec earlier."

		firstPerfVal = perfRun[0][1]
		perfRunNorm = []
		strippedValCtr = 0
		for elem in perfRun:
			normTime = elem[0]/1000-baseTime
			if normTime < 0:
				strippedValCtr += 1
			else:
				perfRunNorm.append([normTime, elem[1]])

		firstCostVal = costRun[0][1]
		lastValInEval = None
		costRunNorm = []
		for elem in costRun:
			normTime = elem[0]-baseTime
			normCostVal = self.normalizeCostVal(elem[1], firstCostVal)
			if normTime < 15/intTimeUnit:
				lastValInEval = normCostVal
			costRunNorm.append([normTime, normCostVal])

		return perfRunNorm, costRunNorm

	def getRTTElement(self, valueInQ):
		#valueInQ is a TimeQuantizedElement
		if valueInQ.perfRTT.getNumSamples() < minSamplesConsider:
			return float('NaN'), None
		return valueInQ.perfRTT.getMean(), valueInQ.perfRTT

	def getPLossElement(self, valueInQ):
		#valueInQ is a TimeQuantizedElement
		if valueInQ.perfRTT.getNumSamples() < minSamplesConsider:
			return float('NaN'), None
		return valueInQ.lossRatio.getMean(), valueInQ.lossRatio


	def getPrefixInLog(self, runid, repeatNo, prefix):
		filename = inDir + "/" + runid + "/run_vm1_" + runid + "_" + str(repeatNo) + ".log"
		output, err = subprocess.Popen(["/bin/grep", prefix, filename], stdout=subprocess.PIPE).communicate()
		if err != None and err != '':
			print "Grep returned an error: '" + err + "'"
		if output == '':
			return -1
		
		if not output.startswith(prefix):
			print "Grep returned malformed string."
   		return int(output[len(prefix):])

	def finalizeOneRun(self, runid, runsToCheck):
		
		print "Finalizing " + runid

		q = TimeQuantizer(TIME_QUANTIZATION)

		mig_duration = StreamStat()
		mig_success = StreamStat()
		mig_succ_ovfl = StreamStat()
		wqlen = StreamStat()

		# Smaller SLA, larger value.

		sla_array = ["rcvd_packets", "sla15", "sla20", "sla30", "sla50"]
		sla_quota = {}
		for desc in sla_array:
			sla_quota[desc] = SLAQuota(desc)

		costs = []
		perfs = []

		for run in runsToCheck:
			elem = self.getPrefixInLog(runid, run, "mig_duration_usec=")
			if (elem < 0):
				#The migration did not succeed. We should not consider costs/perfs these measurements for plotting.
				mig_success.addValue(0.0)
				mig_succ_ovfl.addValue(0.0)
			else:
				mig_success.addValue(1.0)

				if self.getPrefixInLog(runid, run, "overflown_pkt_q=") == 0:

					mig_duration.addValue(elem)	#really?

					mig_succ_ovfl.addValue(1.0)

					costs.append(self.getFromJSON(inDir + "/" + runid + "/sw_host_load_" + str(run) + ".json"))
					perfs.append(self.getFromJSON(inDir + "/" + runid + "/performance_" + str(run) + ".json"))

				else: 
					mig_succ_ovfl.addValue(0.0)
					
			wqlen0 = self.getPrefixInLog(runid, run, "waitq_handled_packets_0=")
			wqlen1 = self.getPrefixInLog(runid, run, "waitq_handled_packets_0=")
#			print "(" + str(wqlen0) + "|" + str(wqlen1) + ")"
			if (wqlen0 < 0):
			   wqlen0 = 0
			if (wqlen1 < 0):
			   wqlen1 = 0
			   
			wqlen.addValue(wqlen0 + wqlen1)
			

		i=0
		for costRun in costs:
			perfRunNorm, costRunNorm = self.getRunNormalized(costs[i], perfs[i])

			for desc, sla in sla_quota.iteritems():
				sla.addRun(perfRunNorm)

			q.addAllPerf(perfRunNorm)
			q.addAllCost(costRunNorm)
			i += 1


			

		lastQValue = q.getNextSmallerEq(10/intTimeUnit)
		#print "Getting results of q value " + str(lastQValue[0])
		#print lastQValue[1].mgtBytesVM1.getValsCSV()
		voiVals = lastQValue[1].mgtBytesVM1.getValsCSV() if lastQValue[1] else ["NaN", "NaN", "NaN"] 	# (mgtBytesVM1 (avg, ci, n)). Here, append more if needed.

		largestRTTKey, largestMeanRTT = q.getLargestElement(self.getRTTElement)
		largestPLossKey, largestPLossRTT = q.getLargestElement(self.getPLossElement)
		#pp.pprint(voiVals)
		#pp.pprint(largestMeanRTT.perfRTT.getValsCSV())
		voiVals += largestMeanRTT.perfRTT.getValsCSV() 	#5
		voiVals += largestPLossRTT.lossRatio.getValsCSV() #8
		voiVals += mig_duration.getValsCSV() #11
		voiVals += mig_success.getValsCSV() #14
		voiVals += mig_succ_ovfl.getValsCSV() #17
		voiVals += wqlen.getValsCSV() #20
		
		
		for typ3 in sla_array:
			voiVals += sla_quota[typ3].absoluteVals.getValsCSV()
			voiVals += sla_quota[typ3].relativeVals.getValsCSV()

		#pp.pprint(voiVals)

		q.writeToCSVFile(outDir + "/results_time_" + runid + ".csv")

		return voiVals


	def writeVOIVals(self, xUnit, vals, f):
		f.write("\t".join([str(xUnit) ,] + vals) + "\n")


	def startNAT(self):

		repeats = range(1,20)

		bwVoiDefDL1400 = open(outDir + "/voi_default_bws_pkt1400_dl05.csv", "w")
		bwVoiLgcyDL1400 = open(outDir + "/voi_legacy_bws_pkt1400_dl05.csv", "w")
		bwVoiDefDL512 = open(outDir + "/voi_default_bws_pkt512_dl05.csv", "w")
		bwVoiLgcyDL512 = open(outDir + "/voi_legacy_bws_pkt512_dl05.csv", "w")
#		bwVoiDefDLNoSnapSz = open(outDir + "/voi_default_bws_dl05_nosnapsz.csv", "w")
#		bwVoiLgcyDLNoSnapSz = open(outDir + "/voi_legacy_bws_dl05_nosnapsz.csv", "w")

		for bw in [50,100,150,200,250,300,350,400,450]:
			self.writeVOIVals(bw*2, self.finalizeOneRun("default_bw" + str(bw) + "_pkt1400_dl05", repeats), bwVoiDefDL1400)
			self.writeVOIVals(bw*2, self.finalizeOneRun("legacy_bw" + str(bw) + "_pkt1400_dl05", repeats), bwVoiLgcyDL1400)

			self.writeVOIVals(bw*2, self.finalizeOneRun("default_bw" + str(bw) + "_pkt512_dl05", repeats), bwVoiDefDL512)
			self.writeVOIVals(bw*2, self.finalizeOneRun("legacy_bw" + str(bw) + "_pkt512_dl05", repeats), bwVoiLgcyDL512)

			#self.writeVOIVals(bw*2, self.finalizeOneRun("default_bw" + str(bw) + "_dl05_nosnapsz", range(repeats)), bwVoiDefDLNoSnapSz)
			#self.writeVOIVals(bw*2, self.finalizeOneRun("legacy_bw" + str(bw) + "_dl05_nosnapsz", range(repeats)), bwVoiLgcyDLNoSnapSz)


		bwVoiDefDL1400.close()
		bwVoiLgcyDL1400.close()
		bwVoiDefDL512.close()
		bwVoiLgcyDL512.close()

#		bwVoiDefDLNoSnapSz.close()
#		bwVoiLgcyDLNoSnapSz.close()

	def startHandover(self):

		repeats = range(1,20)

		bwVoiDefDL1400 = open(outDir + "/voi_default_bws_pkt1400_dl05.csv", "w")
		bwVoiLgcyDL1400 = open(outDir + "/voi_legacy_bws_pkt1400_dl05.csv", "w")
		bwVoiDefDL512 = open(outDir + "/voi_default_bws_pkt512_dl05.csv", "w")
		bwVoiLgcyDL512 = open(outDir + "/voi_legacy_bws_pkt512_dl05.csv", "w")
#		bwVoiDefDLNoSnapSz = open(outDir + "/voi_default_bws_dl05_nosnapsz.csv", "w")
#		bwVoiLgcyDLNoSnapSz = open(outDir + "/voi_legacy_bws_dl05_nosnapsz.csv", "w")

		for bw in [50,100,150,200,250,300,350,400,450]:
			self.writeVOIVals(bw*2, self.finalizeOneRun("default_bw" + str(bw) + "_pkt1400_dl05", repeats), bwVoiDefDL1400)
			self.writeVOIVals(bw*2, self.finalizeOneRun("legacy_bw" + str(bw) + "_pkt1400_dl05", repeats), bwVoiLgcyDL1400)

			self.writeVOIVals(bw*2, self.finalizeOneRun("default_bw" + str(bw) + "_pkt512_dl05", repeats), bwVoiDefDL512)
			self.writeVOIVals(bw*2, self.finalizeOneRun("legacy_bw" + str(bw) + "_pkt512_dl05", repeats), bwVoiLgcyDL512)

			#self.writeVOIVals(bw*2, self.finalizeOneRun("default_bw" + str(bw) + "_dl05_nosnapsz", range(repeats)), bwVoiDefDLNoSnapSz)
			#self.writeVOIVals(bw*2, self.finalizeOneRun("legacy_bw" + str(bw) + "_dl05_nosnapsz", range(repeats)), bwVoiLgcyDLNoSnapSz)


		bwVoiDefDL1400.close()
		bwVoiLgcyDL1400.close()
		bwVoiDefDL512.close()
		bwVoiLgcyDL512.close()

#		bwVoiDefDLNoSnapSz.close()
#		bwVoiLgcyDLNoSnapSz.close()


app = FinalizerMain()

#NAT NF
inDir = "../../slim-data/rc2/smpl"
outDir = "../../slim-data/outPlottable_nat"
app.startNAT()

#PG NF
inDir = "../../slim-data/rc2_handover/smpl"
outDir = "../../slim-data/outPlottable_handover"
app.startHandover()























