#!/usr/bin/env python3
"""Compare capture plans with unmodified functions from a pinned audio driver."""
import argparse
import hashlib
import pathlib
import re
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source-dir', type=pathlib.Path, required=True)
parser.add_argument('--build-dir', type=pathlib.Path, required=True)
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parents[1]
args.build_dir.mkdir(parents=True, exist_ok=True)
expected = {
    'snd_sunxi_i2s.c': 'd3446ec1e11a3fd69d53161f5612a0155e43391ee14569f1fecf942d7daec3a3',
    'snd_sunxi_i2s.h': 'f5eeda49997321150059b1c87cb016c7e2f3ed2e68a9c0abe3d0f5349b9b2a02',
}
source = {}
for name, digest in expected.items():
    data = (args.source_dir / name).read_bytes()
    if hashlib.sha256(data).hexdigest() != digest:
        raise SystemExit(f'Pinned input mismatch: {name}')
    source[name] = data.decode()


def extract(name):
    text = source['snd_sunxi_i2s.c']
    start = text.index('static int ' + name + '(')
    opening = text.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


# Preserve original license notices in the temporary file that contains source.
notice = source['snd_sunxi_i2s.c'].split('#define SUNXI_MODNAME')[0]
header = source['snd_sunxi_i2s.h'].split('struct sunxi_i2s;')[0]
macros = '\n'.join(line for line in header.splitlines()
                   if line.startswith('#define') and not line.rstrip().endswith('\\'))
stubs = r'''
#include "a733_i2s.h"
#include <assert.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
typedef uint32_t u32;
#define IS_ERR_OR_NULL(x) (!(x))
#define SND_LOG_DEBUG(...) ((void)0)
#define SND_LOG_INFO(...) ((void)0)
#define SND_LOG_ERR(...) ((void)0)
#define SND_LOG_ERR_STD(...) ((void)0)
enum {SNDRV_PCM_STREAM_PLAYBACK, SNDRV_PCM_STREAM_CAPTURE};
enum {SND_SOC_DAIFMT_I2S=1, SND_SOC_DAIFMT_RIGHT_J, SND_SOC_DAIFMT_LEFT_J,
      SND_SOC_DAIFMT_DSP_A, SND_SOC_DAIFMT_DSP_B};
enum {SUNXI_DAI_HDMI_TYPE=9, HDMI_FMT_PCM=0};
enum {SUNXI_I2S_DAI_FMT, SUNXI_I2S_DAI_SLOT_NUM, SUNXI_I2S_DAI_SLOT_WIDTH};
struct regmap {u32 words[0xa0/4];};
struct sunxi_i2s_dts {
    u32 clk_mode_bclk_freq, clk_mode_lrck_freq, clk_mode_slots, clk_mode_slot_width;
    u32 tx_pin[4], tx_pin_chmap[4][16], rxfifo_chmap[16], rxfifo_pinmap[16], dai_type;
};
struct sunxi_i2s_dai_fmt {u32 data_late, rx_lsb_first, tx_lsb_first, values[3];};
struct sunxi_i2s {
    struct {struct regmap *regmap;} mem;
    struct sunxi_i2s_dts dts;
    struct sunxi_i2s_dai_fmt i2s_dai_fmt;
    struct {u32 hdmi_fmt;} playback_dma_param;
};
struct snd_soc_dai {struct sunxi_i2s *i2s;};
static struct sunxi_i2s *snd_soc_dai_get_drvdata(struct snd_soc_dai *d) {return d->i2s;}
static void regmap_write(struct regmap *m, u32 a, u32 v) {assert(a<0xa0);m->words[a/4]=v;}
static void regmap_update_bits(struct regmap *m, u32 a, u32 mask, u32 v)
{assert(a<0xa0); m->words[a/4]=(m->words[a/4]&~mask)|(v&mask);}
static int sunxi_get_i2s_dai_fmt(struct sunxi_i2s_dai_fmt *f, int n, u32 *v)
{*v=f->values[n];return 0;}
static int sunxi_set_i2s_dai_fmt(struct sunxi_i2s_dai_fmt *f, int n, u32 v)
{f->values[n]=v;return 0;}
'''
functions = '\n\n'.join(extract(name) for name in [
    'sunxi_i2s_dai_set_bclk_ratio', 'sunxi_i2s_set_ch_en',
    'sunxi_i2s_set_ch_map', 'sunxi_i2s_set_daifmt_fmt', 'sunxi_i2s_dai_set_tdm_slot'])
main = r'''
int main(void)
{
    unsigned accepted=0, refused=0;
    for (unsigned ratio=1;ratio<=193;++ratio)
      for (unsigned slot=16;slot<=32;slot+=16)
       for (unsigned bits=16;bits<=slot;bits+=16)
        for (unsigned mode=0;mode<3;++mode) {
        struct regmap map={0}; struct sunxi_i2s source={0};
        source.mem.regmap=&map; source.i2s_dai_fmt.data_late=1;
        source.i2s_dai_fmt.values[SUNXI_I2S_DAI_FMT]=SND_SOC_DAIFMT_I2S;
        struct snd_soc_dai dai={&source};
        unsigned channels=mode==2 ? 2 : 1, first=mode==1 ? 1 : 0;
        source.dts.rxfifo_chmap[0]=first;
        source.dts.rxfifo_chmap[1]=channels==2 ? 1 : 0;
        struct a733_i2s_format f={16000*2*slot*ratio,16000,0,bits,slot,channels,first};
        struct a733_i2s_plan p, before;
        memset(&p,0xa5,sizeof(p)); before=p;
        int reference=sunxi_i2s_dai_set_bclk_ratio(&dai,ratio);
        int native=a733_i2s_plan(&f,&p);
        assert(native == (reference==0));
        if (!native) {assert(!memcmp(&p,&before,sizeof(p)));++refused;continue;}
        assert(!sunxi_i2s_set_daifmt_fmt(&source,SND_SOC_DAIFMT_I2S));
        assert(!sunxi_i2s_dai_set_tdm_slot(&dai,0,0,2,slot));
        assert(!sunxi_i2s_set_ch_en(&source,SNDRV_PCM_STREAM_CAPTURE,channels));
        assert(!sunxi_i2s_set_ch_map(&source,channels));
        assert(p.clkdiv==map.words[SUNXI_I2S_CLKDIV/4]);
        assert((p.ctl&0x30)==map.words[SUNXI_I2S_CTL/4]);
        assert((p.fmt0&~0x70u)==map.words[SUNXI_I2S_FMT0/4]);
        assert(p.chcfg==map.words[SUNXI_I2S_CHCFG/4]);
        assert(p.rxchsel==map.words[SUNXI_I2S_RXCHSEL/4]);
        assert(p.rxmap3==map.words[SUNXI_I2S_RXCHMAP3/4]);
        assert(!map.words[SUNXI_I2S_RXCHMAP0/4]&&!map.words[SUNXI_I2S_RXCHMAP1/4]&&
               !map.words[SUNXI_I2S_RXCHMAP2/4]);
        ++accepted;
    }
    struct a733_i2s_format f={24576000,16000,12288000,32,32,1,1};
    struct a733_i2s_plan p,before;
    assert(a733_i2s_plan(&f,&p));assert(p.clkdiv==0x182 && p.rxmap3==1 && p.pb_mask==0x170);
    assert(p.fmt0==0x1f77 && p.dma_width==4 && p.frame_bytes==4);
    before=p;f.module_hz++;assert(!a733_i2s_plan(&f,&p)&&!memcmp(&before,&p,sizeof(p)));
    f.module_hz--;f.container_bits=24;assert(!a733_i2s_plan(&f,&p));
    f.container_bits=32;f.channels=2;assert(!a733_i2s_plan(&f,&p));
    f.first_slot=0;f.mclk_hz=13000000;assert(!a733_i2s_plan(&f,&p));
    printf("I2S pinned reference: %u supported plans and %u refused ratios passed\n",accepted,refused);
}
'''
file = args.build_dir / 'reference.c'
file.write_text(notice + '\n' + stubs + '\n' + macros + '\n' + functions + '\n' + main)
binary = args.build_dir / 'reference'
subprocess.run(['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                '-Wno-unused-parameter', '-fsanitize=undefined,address',
                '-I' + str(root / 'include'), str(file),
                str(root / 'drivers/audio/a733_i2s.c'),
                str(root / 'drivers/dma/a733_dma.c'),
                str(root / 'drivers/dma/a733_dma_controller.c'), '-o', str(binary)], check=True)
subprocess.run([str(binary)], check=True)
