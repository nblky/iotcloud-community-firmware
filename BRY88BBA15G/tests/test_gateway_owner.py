"""Run the production gateway initialization and UART reply with NVS faults."""
from pathlib import Path
import subprocess
import tempfile
import unittest


class GatewayOwner(unittest.TestCase):
    def test_nvs_identity_and_stock_reply(self):
        source = (Path(__file__).resolve().parents[1] / 'runtime.inc').read_text()
        init = source[source.index('static void screenGatewayInit()'):
                      source.index('static void screenInit()')]
        start = source.index('else if (cmd == "gateway_mac?") {')
        reply = source[source.index('{', start) + 1:
                       source.index('  } else if (cmd == "wifi_state?")', start)]
        declarations = '\n'.join(line for line in source.splitlines()
                                 if line.startswith('static ') and
                                 ('screenGatewaySource =' in line or
                                  'SCREEN_DEFAULT_GATEWAY =' in line))
        program = r'''
#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
using String = std::string;
using nvs_handle_t = int;
using esp_err_t = int;
static const int ESP_OK = 0, NVS_READONLY = 1;
static String screenGateway, wire, logOutput;
static int openError, readError, closes;
static size_t savedLength = 6;
static uint8_t saved[6];
static int nvs_open(const char *space, int mode, nvs_handle_t *handle) {
  assert(!strcmp(space, "sensor3") && mode == NVS_READONLY);
  *handle = 42;
  return openError;
}
static int nvs_get_blob(nvs_handle_t handle, const char *key, void *out, size_t *size) {
  assert(handle == 42 && !strcmp(key, "ipbox_mac") && *size == 6);
  memcpy(out, saved, savedLength < 6 ? savedLength : 6);
  *size = savedLength;
  return readError;
}
static void nvs_close(nvs_handle_t handle) { assert(handle == 42); ++closes; }
static void screenSend(const String &value) { wire = value; }
static struct {
  void printf(const char *format, ...) {
    char text[128];
    va_list args; va_start(args, format);
    vsnprintf(text, sizeof(text), format, args); va_end(args);
    logOutput = text;
  }
} Serial;
'''
        program += declarations + '\n' + init
        program += '\nstatic void gatewayQuery() {\n' + reply + '\n}\n'
        program += r'''
static void check(const char *expected, const char *origin) {
  int before = closes;
  screenGatewayInit();
  assert(closes == before + (openError == ESP_OK ? 1 : 0));
  assert(screenGateway == expected && !strcmp(screenGatewaySource, origin));
  gatewayQuery();
  assert(wire == String("+gateway_mac:") + expected + "\r\n");
  assert(logOutput == String("[G3-GW] gateway MAC=") + expected + " source=" + origin + "\n");
}
int main() {
  // Missing namespace, missing key/type mismatch, and failed partial reads.
  openError = 1; check("020000000001", "fallback");
  openError = 0; readError = 1; saved[0] = 0x02;
  check("020000000001", "fallback");
  readError = 0;
  // Invalid saved sizes, including success with a short blob.
  savedLength = 5; check("020000000001", "fallback");
  savedLength = 7; readError = 1; check("020000000001", "fallback");
  savedLength = 6; readError = 0;
  memset(saved, 0, 6); check("020000000001", "fallback");
  memset(saved, 0xff, 6); check("020000000001", "fallback");
  saved[0] = 0x01; check("020000000001", "fallback");
  // Preserve both globally and locally administered unicast identities.
  const uint8_t valid[] = {0x10, 0xab, 0xcd, 0xef, 0x12, 0x34};
  memcpy(saved, valid, 6); check("10ABCDEF1234", "nvs");
  saved[0] = 0x02; check("02ABCDEF1234", "nvs");
  // Reinitialization cannot retain a stale saved identity after a read fails.
  openError = 1; check("020000000001", "fallback");
  puts("Gateway owner: missing/invalid NVS, preserved unicast identity and UART replies passed");
}
'''
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            cpp, exe = directory / 'gateway.cpp', directory / 'gateway'
            cpp.write_text(program)
            subprocess.run(['c++', '-std=c++11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                            '-no-pie', str(cpp), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()
