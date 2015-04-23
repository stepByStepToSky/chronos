#! /usr/bin/python2.7

from flask import Flask
import logging
import json
import requests
import threading
import os
import sys
import shutil
import signal
from subprocess import Popen
from time import sleep
from collections import namedtuple
from textwrap import dedent
import unittest

# Test the resynchronization operations for Chronos. 
# These tests use multiple Chronos processes that run on the same machine.
# The tests set up a Chronos cluster and add timers to it. They then
# perform scaling operations, and check that the correct number of 
# timers still pop
CHRONOS_BINARY = 'build/bin/chronos'
CONFIG_FILE_PATTERN = 'scripts/log/chronos.livetest.conf%i'
LOG_FILE_DIR = 'scripts/log/'
LOG_FILE_PATTERN = LOG_FILE_DIR + 'chronos%s'

Node = namedtuple('Node', 'ip port')
flask_server = Node(ip='127.0.0.10', port='5001')
chronos_nodes = [
    Node(ip='127.0.0.11', port='7253'),
    Node(ip='127.0.0.12', port='7254'),
    Node(ip='127.0.0.13', port='7255'),
    Node(ip='127.0.0.14', port='7256'),
]

receiveCount = 0
processes = []

# Create log folders for each Chronos process. These are useful for 
# debugging any problems. Running the tests deletes the logs from the 
# previous run
for file_name in os.listdir(LOG_FILE_DIR):
    file_path = os.path.join(LOG_FILE_DIR, file_name)
    if os.path.isfile(file_path) and file_path != (LOG_FILE_DIR + '.gitignore'):
	os.unlink(file_path)
    elif os.path.isdir(file_path):
        shutil.rmtree(file_path)
for node in chronos_nodes:
    log_path = LOG_FILE_PATTERN % node.port
    os.mkdir(log_path)

# Raise the logging level of the Flask app, to silence it during normal tests
log = logging.getLogger('werkzeug')
log.setLevel(logging.ERROR)

# Open /dev/null to redirect stdout and stderr of chronos. This avoids spamming
# the console during tests - comment this out to get the logs when debugging
FNULL = open(os.devnull, 'w')

# The Flask app. This is used to make timer requests and receive timer pops
app = Flask(__name__)

@app.route('/pop', methods=['POST'])
def pop():
    global receiveCount
    receiveCount += 1
    return 'success'

def run_app():
    app.run(host=flask_server.ip, port=flask_server.port)

# Helper functions for the Chronos tests
def start_nodes(lower, upper):
    # Start nodes with indexes [lower, upper) and allow them time to start
    for i in range(lower, upper):
        processes.append(Popen([CHRONOS_BINARY, '--config-file', CONFIG_FILE_PATTERN % i],
                               stdout=FNULL, stderr=FNULL))
    
    sleep(2) 

def kill_nodes(lower, upper):
    # kill nodes with indexes [lower, upper)
    for p in processes[lower: upper]:
        p.kill()

def node_reload_config(lower, upper):
    # SIGHUP nodes with indexes [lower, upper)
    for p in processes[lower: upper]:
        os.kill(p.pid, signal.SIGHUP)
    sleep(2)

def node_trigger_scaling(lower, upper):
    # SIGHUSR1 nodes with indexes [lower, upper)
    for p in processes[lower: upper]:
        os.kill(p.pid, signal.SIGUSR1)
    sleep(2)

def create_timers(target, num):
    # Create and send timer requests. These are all sent to the first Chronos
    # process which will replicate the timers out to the other Chronos processes
    body_dict = {
        'timing': {
            'interval': 10,
            'repeat_for': 10,
        },
        'callback': {
            'http': {
                'uri': 'http://%s:%s/pop' % (flask_server.ip, flask_server.port),
                'opaque': 'stuff',
            }
        }
    }

    for i in range(num):
        r = requests.post('http://%s:%s/timers' % (target.ip, target.port),
                          data=json.dumps(body_dict)
                          )
        assert r.status_code == 200, 'Received unexpected status code: %i' % r.status_code

def write_conf(filename, this_node, nodes, leaving):
    # Create a configuration file for a chronos process 
    log_path = LOG_FILE_PATTERN % this_node.port
    with open(filename, 'w') as f:
        f.write(dedent("""
        [http]
        bind-address = {this_node.ip}
        bind-port = {this_node.port}

        [logging]
        folder = {log_path}
        level = 5

        [cluster]
        localhost = {this_node.ip}:{this_node.port}
        """).format(**locals()))
        for node in nodes:
            f.write('node = {node.ip}:{node.port}\n'.format(**locals()))
        for node in leaving:
            f.write('leaving = {node.ip}:{node.port}\n'.format(**locals()))


# Test the resynchronization operations for Chronos.
class ChronosLiveTests(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        # Start the flask app in its own thread
        threads = []
        t = threading.Thread(target=run_app)
        t.daemon = True
        threads.append(t)
        t.start()
        sleep(1)

    def setUp(self):
        # Track the Chronos processes and timer pops
        global receiveCount
        global processes
        receiveCount = 0
        processes = []

    def tearDown(self):
        # Kill all the Chronos processes
        kill_nodes(0, len(processes))

    def assert_enough_timers_received(self, expected_number):
        # Check that enough timers pop as expected. 
        # This should typically be as many as were added in the first place. 
        # Ideally, we'd be checking where the timers popped from, but that's
        # not possible with these tests (as everything looks like it comes
        # from 127.0.0.1)
        self.assertGreaterEqual(receiveCount, 
                                expected_number,
                               ('Incorrect number of popped timers: received %i, expected at least %i' %
                               (receiveCount, expected_number)))

    def write_config_for_nodes(self, lower, upper):
        # Write configuration files for the nodes
        for num in range(lower, upper):
            write_conf(CONFIG_FILE_PATTERN % num, 
                       chronos_nodes[num], 
                       chronos_nodes[:upper],
                       [])

    def write_scale_down_config_for_all_nodes(self, leaving_lower, leaving_upper):
        # Write configuration files including leaving nodes
        for num in range(len(chronos_nodes)):
            write_conf(CONFIG_FILE_PATTERN % num, chronos_nodes[num],
                       chronos_nodes[:leaving_lower] + chronos_nodes[leaving_upper:],
                       chronos_nodes[leaving_lower: leaving_upper])

    def test_scale_up(self):
        # Test that scaling up works. This test creates 2 Chronos nodes, 
        # adds 100 timers, scales up to 4 Chronos nodes, then checks that
        # 100 timers pop.
 
        # Start initial nodes and add timers
        self.write_config_for_nodes(0, 2)
        start_nodes(0, 2)
        create_timers(chronos_nodes[0], 100)

        # Scale up
        self.write_config_for_nodes(0, 4)
        start_nodes(2, 4)
        node_reload_config(0, 2)
        node_trigger_scaling(0, 4)

        # Check that all the timers have popped
        sleep(10)
        self.assert_enough_timers_received(100)

    def test_scale_up_and_kill(self):
        # Test that scaling up definitely moves timers. This test creates 2 
        # Chronos nodes and adds 100 timers. It then scales up to 4 Chronos 
        # nodes, then kills the first two nodes. It then checks at least 50
        # timers still pop (we'd expect around 75 would pop but this isn't
        # guaranteed. We check 50 so that the test is very unlikely to fail
        # but it also can't pass unless the timers have moved). 

        # Start initial nodes and add timers
        self.write_config_for_nodes(0, 2)
        start_nodes(0, 2)
        create_timers(chronos_nodes[0], 100)

        # Scale up
        self.write_config_for_nodes(0, 4)
        start_nodes(2, 4)
        node_reload_config(0, 2)
        node_trigger_scaling(0, 4)

        # Now kill the first nodes
        kill_nodes(0, 2)

        # Check that all the timers have popped
        sleep(10)
        self.assert_enough_timers_received(50)

    def test_scale_down(self):
        # Test that scaling down works. This test creates 4 Chronos nodes,
        # adds 100 timers, scales down to 2 Chronos nodes, then checks that
        # 100 timers pop.

        # Start initial nodes and add timers
        self.write_config_for_nodes(0, 4)
        start_nodes(0, 4)
        create_timers(chronos_nodes[0], 100)

        # Scale down
        self.write_scale_down_config_for_all_nodes(2, 4)
        node_reload_config(0, 4)
        node_trigger_scaling(0, 4)
        kill_nodes(2, 4)

        # Check that all the timers have popped
        sleep(10)
        self.assert_enough_timers_received(100)

    def test_upscale_downscale(self):
        # Test a scale up and scale down. This test creates 2 Chronos nodes,
        # and adds 100 timers. It then scales up to 4 Chronos nodes, then
        # removes the initial 2 nodes by doing a scale down. It then checks that
        # 100 timers pop.

        # Start initial nodes and add timers
        self.write_config_for_nodes(0, 2)
        start_nodes(0, 2)
        create_timers(chronos_nodes[0], 100)

        # Scale up
        self.write_config_for_nodes(0, 4)
        start_nodes(2, 4)
        node_reload_config(0, 2)
        node_trigger_scaling(0, 4)

        # Scale down the initial nodes
        self.write_scale_down_config_for_all_nodes(0, 2)
        node_reload_config(0, 4)
        node_trigger_scaling(0, 4)
        kill_nodes(0, 2)

        # Check that all the timers have popped
        sleep(10)
        self.assert_enough_timers_received(100)

if __name__ == '__main__':
    unittest.main()