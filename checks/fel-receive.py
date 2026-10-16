#!/usr/bin/env python3
"""Compile the patched FEL receive function against bounded and short-response fixtures."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('source', type=Path)
args = parser.parse_args()
text = args.source.read_text()
start = text.index('static int usb_bulk_recv(')
end = text.index('\nstruct aw_usb_request', start)
fixture = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
typedef void libusb_device_handle;
static const int AW_USB_MAX_BULK_SEND = 512 * 1024;
#define USB_TIMEOUT 10000
static int remaining, short_response, calls;
static void usb_error(int rc, const char *where, int code) { (void)rc; (void)where; exit(code); }
static int libusb_bulk_transfer(void *usb, int ep, unsigned char *data, int length,
                               int *received, unsigned timeout) {
    (void)usb; (void)ep; (void)data; (void)timeout;
    assert(length <= 512 * 1024);
    ++calls;
    *received = short_response ? 8 : length;
    assert(*received <= remaining);
    remaining -= *received;
    return 0;
}
'''
fixture += text[start:end]
fixture += r'''
int main(void) {
    unsigned char *data = malloc(14346688);
    assert(data);
    remaining = 14346688;
    assert(usb_bulk_recv(NULL, 0x81, data, remaining) == 14346688);
    assert(remaining == 0 && calls == 28);
    remaining = 21; short_response = 1; calls = 0;
    assert(usb_bulk_recv(NULL, 0x81, data, 1024) == 8);
    assert(remaining == 13 && calls == 1); /* AWUS must remain unread. */
    free(data);
    puts("PASS: bounded large read and short status preserve protocol boundaries.");
}
'''
with tempfile.TemporaryDirectory() as directory:
    source = Path(directory) / 'check.c'; source.write_text(fixture)
    binary = Path(directory) / 'check'
    subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', '-Werror', str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
