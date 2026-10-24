import unittest
from test_regressions import ROOT, function, run_c

class UartShutdownTests(unittest.TestCase):
    def test_shutdown_detaches_tx_before_releasing_pins(self):
        run_c(r'''
#include <assert.h>
#define GDO_UART_TX_PIN 1
#define GDO_UART_RX_PIN 2
#define SIG_GPIO_OUT_IDX 256
#define GPIO_MODE_INPUT 1
#define false 0
static int step;
static void esp_rom_gpio_connect_out_signal(int pin,int sig,int invert,int oe) {
 assert(step++==0);assert(pin==1 && sig==256 && !invert && !oe);
}
static int gpio_set_direction(int pin,int mode) {
 assert(mode==GPIO_MODE_INPUT);
 assert((step==1 && pin==1)||(step==2 && pin==2)); ++step;return 0;
}
''' + function('main/gdo-blaq-homekit.cpp','static void gdo_uart_shutdown(void)') + r'''
int main(void){gdo_uart_shutdown();assert(step==3);step=0;gdo_uart_shutdown();assert(step==3);}
''', cpp=True)

    def test_shutdown_registered_before_uart_is_connected(self):
        src=function('main/gdo-blaq-homekit.cpp','extern "C" void app_main(void)')
        registration='ESP_ERROR_CHECK(esp_register_shutdown_handler(gdo_uart_shutdown))'
        self.assertLess(src.index(registration),src.index('gdo_init(&gdo_conf)'))
        self.assertIn('gdo_conf.uart_tx_pin    = GDO_UART_TX_PIN;',src)
        self.assertIn('gdo_conf.uart_rx_pin    = GDO_UART_RX_PIN;',src)
