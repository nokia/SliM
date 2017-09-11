from threading import Thread
import socket
import atexit
import sys
import pprint
import httplib
import struct
import traceback

from topo import grt_topo

SERVER_IP = '0.0.0.0' #Bind address, of course.
SERVER_PORT = 1400
OFCTRL_HOST = '172.16.3.9'
OFCTRL_PORT = 8080

CTRL_MSG_POKE		=	10	#2 padding bits and 4 bytes poke UID. Is sent to the client from ctrl to test connection
CTRL_MSG_STATE_TRANSFER =	11	#2 padding bits + 4bytes destination IP	Is sent to the VM from the server.
CTRL_MSG_STUPD_OK	=	12	#6 padding bits	Is sent to the controller from the client.
CTRL_MSG_INIT		=	13	#6 padding bits	Is sent to the client from ctrl to clear and start fwding.

pp = pprint.PrettyPrinter(indent=4)
pp.pprint(grt_topo)

class ConnectedVM:
	def __init__(self, ctrl, name, conn, addr, vminfo):
		self.ctrl = ctrl		
		self.name = name
		self.conn = conn
		self.vminfo = vminfo
		self.addr = addr

	def handle_listen_thread(self):
		while (1):
			data = self.conn.recv(8)
			if not data: 
				break
			if len(data) < 8:
				print "Packet received that was too small."
				continue
				
			(opcode,) = struct.unpack("!Hxxxxxx", data);
			if opcode == CTRL_MSG_POKE:
				(poke_uid,) = struct.unpack("!xxxxI", data);
				print "Pokeback received! Poke ID: " + str(poke_uid) + "."

			elif opcode == CTRL_MSG_STUPD_OK:
				print self.name + ": State update OK msg received! Redirecting flows now."
				self.migrateStage1()

			#TODO Maybe more receive handling here.

	def poke(self, poke_uid):
		print "Poking " + self.name + " at " + pp.pformat(self.addr) + ", poke uid " + str(poke_uid) + "..."
		self.ctrl.sendMessagePoke(self.conn, poke_uid)

	def initFwd(self):
		print "Initing " + self.name + " at " + pp.pformat(self.addr) + "..."
		self.ctrl.ofRestCommand('/grt/init/' + self.name)
		self.ctrl.sendMessageInit(self.conn)

	def migrateTo(self, otherVM):	#Stage 0
		#TODO
		print "Transferring " + self.name + " at " + pp.pformat(self.addr) + " to " + pp.pformat(otherVM.addr) + "..."
		#sendMessageStateTransfer(self.conn, "64.64.64.64")	#This is for
		self.ctrl.ofRestCommand('/grt/migrate/' + self.name + "/" + otherVM.name + "/0")
		self.ctrl.sendMessageStateTransfer(self.conn, otherVM.addr)
		self.ctrl.migrateMap[otherVM] = self

	def migrateStage1(self):
		sourceVM = self.ctrl.migrateMap[self]
		self.ctrl.ofRestCommand('/grt/migrate/' + sourceVM.name + "/" + self.name + "/1")

	def beforeDisconnect(self):
		self.deinitFwd()

	def deinitFwd(self):
		self.ctrl.ofRestCommand('/grt/deinit/' + self.name)

		

#======== Message handling

class CtrlCmdError(Exception):
	def __init__(self, msg):
		super(CtrlCmdError, self).__init__(msg)

class GRTController():

	def __init__(self):
		self.migrateMap = {}		# Map: destVM -> sourceVM. All ConnectedVM objects.
		self.connected_vms = {}
		self.poke_uid=0;

	def start(self):

		self.t_connection_loop = Thread(target=self.connectionLoop, args=())
		self.t_connection_loop.start()

		try:
			##We run it in main thread.
			#self.t_stdin_loop = Thread(target=self.stdin_loop, args=())
			#self.t_stdin_loop.start()
			self.ofRestCommand('/grt/reinit')

			print("Finished initializing...")
			self.stdin_loop()
		except Exception as e: 
			print("Error in stdin loop, cleaning up nevertheless...")
			print '-'*60
			traceback.print_exc(file=sys.stdout)
			print '-'*60
		
		self.endGracefully()

		

	def ofRestCommand(self, url):
		self.of_rest = httplib.HTTPConnection(OFCTRL_HOST, OFCTRL_PORT)
		self.of_rest.request("POST", url)
		response = self.of_rest.getresponse()
		logAsync("OF REST: Sent command " + url + " got response " + str(response.status) + " " + response.reason)

	#def waitUntilEnd(self):
	#	self.t_stdin_loop.join()
	#	self.t_connection_loop.join()

	def endGracefully(self):
		print "Ending gracefully..."
		self.grts.shutdown(socket.SHUT_RDWR)
		self.grts.close()

	def connectionLoop(self):

		#print("Connection loop started...")
		self.grts = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
		self.grts.bind((SERVER_IP, SERVER_PORT))
		self.grts.listen(1)
		while (1):
			try:
				conn, addr = self.grts.accept()
				t = Thread(target=self.onConnectionOpened, args=(conn, addr))
				t.start()
			except socket.error as msg:
				break
				print "Connection socket closed, exiting."

	def sendMessagePoke(self, conn, poke_uid):
		#2 byte UInt for command
		#2 byte currently unused
		#4 byte poke UID

		byteVector = struct.pack("!HxxI", CTRL_MSG_POKE, poke_uid)
		#print "Sending byte vector: " + byteVector.encode('hex')
		conn.send(byteVector)

	def sendMessageInit(self, conn):
		#2 byte UInt for command
		#6 byte currently unused

		byteVector = struct.pack("!Hxxxxxx", CTRL_MSG_INIT)
		print "Sending byte vector: " + byteVector.encode('hex')
		conn.send(byteVector)

	def sendMessageStateTransfer(self, conn, ipDst):
		#2 byte UInt for command
		#2 byte currently unused
		#4 byte destination IP address

		byteVector = struct.pack("!Hxx", CTRL_MSG_STATE_TRANSFER) + socket.inet_aton(ipDst[0])
		#print "Sending byte vector: " + byteVector.encode('hex')
		conn.send(byteVector)



	#======== Other glob functions

	def onConnectionOpened(self, conn, addr):
		global grt_topo
	
		ourVM = None

		logAsync('Connection received from VM at address: ' +  pp.pformat(addr))

		# Looking if our connected peer is registered at us...
		for name,vminfo in grt_topo.iteritems():
			if name == 'misc':
				continue
			if vminfo["mgmt_ip"] == addr[0]:
				ourVM = ConnectedVM(self,name,conn,addr,vminfo)
				self.connected_vms[name] = ourVM
			
				break;
		if ourVM == None:
			logAsync('The machine is not registered. Closing connection ' +  pp.pformat(addr))
			conn.close(conn)
			return

		logAsync('VM ' + ourVM.name + ' registered.')

		ourVM.handle_listen_thread() #Exits when the connection is closed for whatever reason.
	
		ourVM.beforeDisconnect()
		del self.connected_vms[ourVM.name]
		logAsync('VM ' + ourVM.name + ' unregistered.')
	
		logAsync('Connection closed from VM at address: ' + pp.pformat(addr))









	def stdin_loop(self):
		print("Stdin loop started...")

		self.terminated = False
		while not self.terminated:
			sys.stdout.write('>')
			in_str = sys.stdin.readline()
			try:
				self.handle_cmd(in_str)
			except CtrlCmdError as e:
				print e.message

			#TODO more...

	def handle_cmd(self, in_str):

		parts = in_str.replace('\n', '').replace('\r', '').split(" ")

		if len(parts) <= 1 and parts[0] == "": return

		#print "in_str: '" + in_str + "', parts: '" + str(len(parts)) + "'"
		#pp.pprint(parts)

		if parts[0] == 'm' or parts[0] == 'migrate':
			if len(parts) < 3:
				raise CtrlCmdError("Usage: m <source name> <dest name>")
			else:
				vmSrc = self.connected_vms.get(parts[1])
				if  not vmSrc: 
					raise CtrlCmdError("Could not find src VM " + parts[1] + " (it might not be registered). ")

				vmDst = self.connected_vms.get(parts[2])
				if not vmDst:
					#TODO: Check if the VM is in the correct state.
					raise CtrlCmdError("Could not find dest VM " + parts[2] + " (it might not be registered). ")

				vmSrc.migrateTo(vmDst)


		elif parts[0] == 'p' or parts[0] == 'poke':
			if len(parts) < 2:
				raise CtrlCmdError("Usage: p <ID>")
			else:
				vm2poke = self.connected_vms.get(parts[1])
				self.poke_uid += 1
				if (vm2poke): vm2poke.poke(self.poke_uid)
				else: raise CtrlCmdError("Could not find VM " + parts[1] + " (it might not be registered). ")
		
		elif parts[0] == 'i' or parts[0] == 'init':
			if len(parts) < 2:
				raise CtrlCmdError("Usage: i <ID>")
			else:
				vm2init = self.connected_vms.get(parts[1])
				if (vm2init): vm2init.initFwd()
				else: raise CtrlCmdError("Could not find VM " + parts[1] + " (it might not be registered). ")
	
		elif parts[0] == 'exit':
			self.terminated = True


		else:
			raise CtrlCmdError("Unknown command: " + parts[0])
		
		return "OK"

		



def logAsync(linestring):
	sys.stdout.write('\b')	#Delete prompt at the end
	sys.stdout.write(linestring)
	sys.stdout.write('\n>')







