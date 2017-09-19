#! /usr/bin/env python

import json
from wsgiref import util
from wsgiref import simple_server
from deps.OFDPA_python import *
#import socket
import dp_master
import re

activeVM = None
nextVM = None

migration_debris = None

def main():
	global conf

	with open('conf-example.json') as conffile: 
		conf = json.load(conffile)

	dp_master.init(conf)

	httpd = simple_server.make_server('', 8080, server)
	httpd.serve_forever()

def do_reinit():
	global activeVM, conf
	print "Requested reinit..."
	if (nextVM != None and activeVM != None):
		print "Last migration seems to have crashed between stages 0 and 1. Trying to go to Stage 1."
		do_migrate(activeVM, nextVM, "1")
	if (activeVM != None):
		do_deinit(activeVM)
	if (nextVM != None and activeVM == None):
		print "ERROR: Impossible state, restart dataplane."

def do_deinit(vmToDeinit):
	global activeVM, conf

	if (activeVM != vmToDeinit):
		print "VM to deinit '" + vmToDeinit + "' is not enabled. Ignoring."
	else:
		cleanup_migration_debris()
		print "Deiniting '" + vmToDeinit + "'"
		dp_master.dp_vnf_deinit(conf, vmToDeinit)
		activeVM = None

def do_init(vmToInit):
	global activeVM, conf
	if activeVM == None:
		print "Initing '" + vmToInit + "'"
		dp_master.dp_vnf_init(conf, vmToInit)
		activeVM = vmToInit
	else:
		print "Cannot init VM as another VM is active."

def do_migrate(srcVM, destVM, stage_str):
	global activeVM, nextVM, conf, migration_debris

	stage = int(stage_str)

	if not stage in [0,1]:
		print "Illegal migration stage."
		return

	if activeVM != srcVM:
		print "Source VM not active."
		return

	if stage == 0:
		if nextVM != None:
			print "Cannot do migrate, migration already in progress..."
			return
		nextVM = destVM
		cleanup_migration_debris()

	if stage == 1:
		if nextVM != destVM or activeVM != srcVM:
			print "Destination or source VM mismatch."
			return
		activeVM = destVM
		nextVM = None
		migration_debris = (srcVM, destVM)

	dp_master.dp_vnf_migrate(conf, srcVM, destVM, stage)

def cleanup_migration_debris():
	global conf, migration_debris

	if migration_debris == None:
		return

	(srcVM, destVM) = migration_debris
	print "Cleaning up flow debris of last migration first..."
	dp_master.dp_vnf_migrate(conf, srcVM, destVM, 2)
	migration_debris = None


def server(env, resp):
    util.setup_testing_defaults(env)

    req = env.get('PATH_INFO', '')
    print "Request: " + req
    req_m = re.match(r'/grt/(.*)', req)
    if req_m != None:
	cmdfull = req_m.group(1)
	params = cmdfull.split('/')

	if (params[0] == "reinit"):
		do_reinit()

	elif (params[0] == "deinit"):
		do_deinit(params[1])

	elif (params[0] == "init"):
		do_init(params[1])

	elif (params[0] == "migrate"):
		do_migrate(params[1], params[2], params[3])

    status = '200 OK'
    headers = [('Content-type', 'text/plain')]

    resp(status, headers)
	
    ret = "Bla Response, URI was: " + req
    return ret




if __name__ == '__main__': main()
