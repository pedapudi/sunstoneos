/*
 * Copyright © 2022, 2025 Imagination Technologies Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
/* Fixed 32-instance numerical workload; shader source is retained in shaders/. */
static const uint32_t probe_usc[70] = {
    0x13409286u, 0x00000020u, 0xff400000u, 0xd3009256u,
    0x81d3803fu, 0x41300010u, 0xd300d299u, 0x9ca1f03cu,
    0xc187871eu, 0x401180cfu, 0x82444020u, 0x00002067u,
    0xc098ff45u, 0xf13cd300u, 0x871e9ca0u, 0x1180cfc0u,
    0x82442000u, 0x00453867u, 0x8004ff45u, 0x00bc0460u,
    0x82350000u, 0x04808700u, 0x40000000u, 0x87008235u,
    0x00000481u, 0xa0654100u, 0x0008f100u, 0xff004040u,
    0xff6a8002u, 0xd3009256u, 0x81d3803fu, 0x42300010u,
    0xe2008236u, 0x010041c2u, 0xff420000u, 0xea008236u,
    0x400061c2u, 0xff400000u, 0x6840b256u, 0x0050900fu,
    0xff410033u, 0xd100d278u, 0x87b0f03cu, 0x80d0c187u,
    0x41010010u, 0x13409286u, 0x9e3779b9u, 0xff420000u,
    0xe2008236u, 0x410041c2u, 0xff410000u, 0x4240b256u,
    0x00408002u, 0xff444140u, 0x0040d256u, 0x01d38002u,
    0xff408280u, 0xe0008636u, 0x40184382u, 0x4382c000u,
    0xf200a065u, 0x42440864u, 0xa054ff00u, 0x4200e000u,
    0x80020000u, 0x8245ff6au, 0x00452467u, 0xfff1ff45u,
    0x00ee8004u, 0xfffffff2u,
};
