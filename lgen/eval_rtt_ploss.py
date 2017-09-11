#Processes the raw data collected by grt-lgen in a binary format to a human-readable format, either CSV or JSON.

import struct
from os import listdir
from os.path import isfile, join
import pprint
import math
import json

pp = pprint.PrettyPrinter(indent=4)
CSV_SEP ='\t'

QUANTIZATION_UNIT = 500*1000*1000 #in nanoseconds

class StreamStat:
	
	def __init__(self):
		self.aggr = 0
		self.m = 0.0
		self.s = 0.0
		self.k = 0

	def addValue(self, val):
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


class GRTLogTimeAggregatedValue:

	def __init__(self):
		self.rtt_stat = StreamStat()
		self.total_packets = 0
		self.sla15 = 0
		self.sla20 = 0
		self.sla30 = 0
		self.sla50 = 0

	@classmethod
	def quantizeTime(cls, sample):
		return ((long)(sample.abs_snd_time/QUANTIZATION_UNIT))*QUANTIZATION_UNIT

	def addSample(self, sample):
		self.total_packets += 1
		if sample.rtt_nsec < 0:
			return
		self.rtt_stat.addValue(sample.rtt_nsec)
		if sample.rtt_nsec < 15 * 1000000:
			self.sla15 += 1
		if sample.rtt_nsec < 20 * 1000000:
			self.sla20 += 1
		if sample.rtt_nsec < 30 * 1000000:
			self.sla30 += 1
		if sample.rtt_nsec < 50 * 1000000:
			self.sla50 += 1

	@classmethod
	def getValuesDescForCSV(cls):
		return "total_packets", "rcvd_packets", "loss_ratio", "rtt_mean", "rtt_stdev", "sla15", "sla20", "sla30", "sla50"

	def getValuesForCSV(self):
		rcvd_packets = self.rtt_stat.getNumSamples()

		return (str(self.total_packets),					#total_packets
			str(rcvd_packets),					#rcvd_packets
			str(1.0-float(rcvd_packets)/self.total_packets),	#loss_ratio
			str(self.rtt_stat.getMean()),				#rtt_mean_nsec
			str(self.rtt_stat.getStdDev()),			#rtt_stdev_nsec
			str(self.sla15),
			str(self.sla20),
			str(self.sla30),
			str(self.sla50))

	def toJSONStruct(self):
		rcvd_packets = self.rtt_stat.getNumSamples()

		return {"total_packets" : self.total_packets, "rcvd_packets" : rcvd_packets, "loss_ratio" : (1.0-float(rcvd_packets)/self.total_packets),
		"rtt_mean_nsec" : self.rtt_stat.getMean(), "rtt_stdev_nsec" : self.rtt_stat.getStdDev(),
		"sla15":self.sla15,"sla20":self.sla20,"sla30":self.sla30,"sla50":self.sla50}
		
		
class GRTLogTimeAggregator:

	def __init__(self):
		self.table = {}

	def addSample(self, sample):
		aggrKey = GRTLogTimeAggregatedValue.quantizeTime(sample)
		if not aggrKey in self.table.keys():
			aggrVal = GRTLogTimeAggregatedValue()
			self.table[aggrKey] = aggrVal
		else:
			aggrVal = self.table[aggrKey]

		aggrVal.addSample(sample)

	def writeCSV(self, outfile):
		outfile.write("#time" + CSV_SEP + CSV_SEP.join(GRTLogTimeAggregatedValue.getValuesDescForCSV()) + "\n")

		for key, value in sorted(self.table.iteritems()):
			outfile.write(str(key) + CSV_SEP + CSV_SEP.join(value.getValuesForCSV()) + '\n')

	def toJSONStruct(self):
		struct = []
		for key, value in sorted(self.table.iteritems()):
			struct.append([key, value.toJSONStruct()])
		return struct
			
		
	

class GRTLogSample:

	def __init__(self, thestruct):
		(self.thread_no, self.packet_c, self.abs_snd_time) = struct.unpack("!HIQ", thestruct)
		self.rtt_nsec = -1

	def __repr__(self):
		return self.__str__()
		
	def __str__(self):
		return "( thread_no=" + str(self.thread_no) + ", packet_c=" + str(self.packet_c) + ", abs_snd_time=" + str(self.abs_snd_time) + ", rtt_nsec=" + str(self.rtt_nsec) + ")"

	#Returns -1, if packet is not for this connection
	def onFoundRcvPacket(self, thestruct):
		(rcv_thread_no, rcv_packet_c, rcv_rtt_nsec) = struct.unpack("!HIQ", thestruct)
		if self.packet_c != rcv_packet_c:
			return -1
		if self.thread_no != rcv_thread_no:
			return -2
		if rcv_rtt_nsec < 0:
			return -3
		self.rtt_nsec = rcv_rtt_nsec
		return 0

	@classmethod
	def getPacketCFromRcvStruct(cls, thestruct):
		(packet_c,) = struct.unpack("!xxIxxxxxxxx", thestruct)
		return packet_c

		

class RttPlossEval:
	def __init__(self, inputDir, outputFile, csv=False):
		files = [join(inputDir, f) for f in listdir(inputDir) if (isfile(join(inputDir, f)) and f.endswith(".grtlog-snd"))]
		#pp.pprint(files)

		self.aggregator = GRTLogTimeAggregator()

		for f in files:
			 self._readGrtLog(f, f[:-3]+"rcv")

		
		if csv:
			self._writeCSV(outputFile)
		else:
			self._writeJSON(outputFile)

	def _readGrtLog(self, infilename_snd, infilename_rcv):
		print(infilename_snd, infilename_rcv)
		f_snd = open(infilename_snd, "r")
		f_rcv = open(infilename_rcv, "r")

		lookBackMap = {}


		while(True):
			snd_bytes = f_snd.read(14)
			if len(snd_bytes) <14:
				break
			newSample = GRTLogSample(snd_bytes)
			lookBackMap[newSample.packet_c] = newSample
			
		while(True):
			rcv_bytes = f_rcv.read(14)
			if len(rcv_bytes) <14:
				break

			key = GRTLogSample.getPacketCFromRcvStruct(rcv_bytes)
			status = lookBackMap[key].onFoundRcvPacket(rcv_bytes)

			if (status < 0):
				print("Illegal state, should not happen")
				return -1

		#pp.pprint(lookBackMap)

		for key,value in lookBackMap.iteritems():
			self.aggregator.addSample(value)

	def _writeCSV(self, outputFile):
		f_aggr = open(outputFile, "w")
		self.aggregator.writeCSV(f_aggr)
		f_aggr.close()

	def _writeJSON(self, outputFile):
		f_aggr = open(outputFile, "w")
		json.dump(self.aggregator.toJSONStruct(), f_aggr, indent=4, sort_keys=True);
		f_aggr.close()
		
		

#RttPlossEval("output/raw", "output/aggr/aggr.csv")













