import unittest
from test_regressions import ROOT, run_c
class RetryTests(unittest.TestCase):
    def test_one_retry_per_command(self):
        run_c('#include <assert.h>\n#include "'+str(ROOT/'main/retry_budget.h')+'"\n'+r'''
int main(void) {
 retry_budget_t b={0};
 assert(retry_budget_step(&b,1,1,0,8000)==RETRY_WAIT);
 assert(retry_budget_step(&b,1,1,9000,8000)==RETRY_SEND);
 assert(retry_budget_step(&b,1,1,18000,8000)==RETRY_REVERT);
 for(int t=19000;t<100000;t+=1000) assert(retry_budget_step(&b,1,1,t,8000)==RETRY_WAIT);
 assert(retry_budget_step(&b,2,1,100000,8000)==RETRY_WAIT);
 assert(retry_budget_step(&b,2,1,109000,8000)==RETRY_SEND);
 // Actual motion, resolution, or cancellation consumes this command.
 assert(retry_budget_step(&b,2,0,110000,8000)==RETRY_WAIT);
 assert(retry_budget_step(&b,2,1,130000,8000)==RETRY_WAIT);
 assert(retry_budget_step(&b,0,1,140000,8000)==RETRY_WAIT);
}''')
