import controller
import pprint
from threading import Thread
from controller import CtrlCmdError
from flask import Flask
from flask import request

# Example usage: curl -X POST 'http://localhost:8081/cmd?cmd=i+vm0' --dump-header headers

pp = pprint.PrettyPrinter(indent=4)

rest_if = Flask(__name__)
#rest_if.debug = True
ctrl = controller.GRTController()

@rest_if.route("/cmd", methods=['POST'])
def execCmd():
	cmd = request.args.get('cmd')

	if cmd == None: return "No command specified\n", 403	

	try:
		print "Executing command '" + cmd + "' via REST..."
		result = ctrl.handle_cmd(cmd)
		return result + '\n'
	except CtrlCmdError as e:
		return e.message + '\n', 403
		
def ctrlThread():
	ctrl.start()	

t_ctrl = Thread(target=ctrlThread, args=())
t_ctrl.start()

rest_if.run(port=8081)

print("Exiting...")






