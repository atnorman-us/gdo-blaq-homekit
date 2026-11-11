import unittest
from test_regressions import ROOT, function, run_c

STUBS = r'''
#include <assert.h>
#define IRAM_ATTR
#define GDO_UART_TX_PIN 1
#define GDO_UART_RX_PIN 2
#define SIG_GPIO_OUT_IDX 256
#define GPIO_MODE_INPUT 1
#define GPIO_PORT_0 0
#define GPIO_HAL_GET_HW(port) ((void *)0)
#define false 0
static int step;
static void esp_rom_gpio_connect_out_signal(int pin,int sig,int invert,int oe) {
 assert(step++==0);assert(pin==1 && sig==256 && !invert && !oe);
}
static void gpio_ll_output_disable(void *hw,int pin) {
 assert(step==1 && pin==1); ++step;
}
static void gpio_ll_pulldown_en(void *hw,int pin) {
 assert(step==2 && pin==1); ++step;
}
'''

RELEASE = function('main/gdo-blaq-homekit.cpp','static void IRAM_ATTR gdo_uart_release_tx(void)')

class UartShutdownTests(unittest.TestCase):
    def test_shutdown_detaches_tx_before_releasing_pins(self):
        run_c(STUBS + r'''
static int gpio_set_direction(int pin,int mode) {
 assert(mode==GPIO_MODE_INPUT);
 assert((step==3 && pin==1)||(step==4 && pin==2)); ++step;return 0;
}
''' + RELEASE + function('main/gdo-blaq-homekit.cpp','static void gdo_uart_shutdown(void)') + r'''
int main(void){gdo_uart_shutdown();assert(step==5);step=0;gdo_uart_shutdown();assert(step==5);}
''', cpp=True)

    def test_panic_wrapper_releases_tx_before_the_real_handler(self):
        run_c(STUBS + r'''
static int real_called;
extern "C" void __real_esp_panic_handler(void *info) {
 assert(step==3 && info==(void *)0x1234); real_called++;
}
''' + RELEASE + function('main/gdo-blaq-homekit.cpp','extern "C" void IRAM_ATTR __wrap_esp_panic_handler(void *info)') + r'''
int main(void){__wrap_esp_panic_handler((void *)0x1234);assert(step==3 && real_called==1);}
''', cpp=True)

    def test_panic_path_stays_in_iram(self):
        source=(ROOT/'main/gdo-blaq-homekit.cpp').read_text()
        self.assertIn('static void IRAM_ATTR gdo_uart_release_tx(void)',source)
        self.assertIn('extern "C" void IRAM_ATTR __wrap_esp_panic_handler(void *info)',source)
        # Only ROM and register-level calls: a panic cannot rely on flash code.
        for call in ('gpio_set_direction','gpio_pulldown_en','ESP_LOG'):
            self.assertNotIn(call,RELEASE)

    def test_panic_handler_is_wrapped_at_link_time(self):
        cmake=(ROOT/'main/CMakeLists.txt').read_text()
        self.assertIn('target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_panic_handler")',cmake)

    def test_shutdown_registered_before_uart_is_connected(self):
        src=function('main/gdo-blaq-homekit.cpp','extern "C" void app_main(void)')
        registration='ESP_ERROR_CHECK(esp_register_shutdown_handler(gdo_uart_shutdown))'
        self.assertLess(src.index(registration),src.index('gdo_init(&gdo_conf)'))
        self.assertIn('gdo_conf.uart_tx_pin    = GDO_UART_TX_PIN;',src)
        self.assertIn('gdo_conf.uart_rx_pin    = GDO_UART_RX_PIN;',src)
