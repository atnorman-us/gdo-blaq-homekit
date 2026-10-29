"""The diagnostics web server restart is called from the WiFi/IP event handler."""
import unittest
from test_regressions import function


class DiagRestartTests(unittest.TestCase):
    def test_restart_does_not_rebuild_the_server_on_the_caller_stack(self):
        # The event loop task has a 2304-byte stack and start_httpd_server()
        # keeps the config and every URI record on the stack. The restart must
        # hand that work to a task with its own stack.
        entry = function('main/diag_webserver.cpp', 'extern "C" void diag_webserver_restart(void)')
        self.assertNotIn('start_httpd_server', entry)
        self.assertNotIn('httpd_stop', entry)
        self.assertIn('xTaskCreate(diag_restart_task', entry)

        worker = function('main/diag_webserver.cpp', 'static void diag_restart_task(void *arg)')
        self.assertIn('start_httpd_server()', worker)

    def test_restart_worker_has_room_for_the_server_setup(self):
        entry = function('main/diag_webserver.cpp', 'extern "C" void diag_webserver_restart(void)')
        stack = int(entry.split('"diag_net_restart", ')[1].split(',')[0])
        self.assertGreaterEqual(stack, 6144)


if __name__ == '__main__':
    unittest.main()
