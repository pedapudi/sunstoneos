#!/usr/bin/env python3
"""Compare native render records with pinned board and Mesa initializers."""
import argparse
import hashlib
import json
import re
import shutil
import subprocess
import tarfile
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SHIM = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64; typedef int32_t s32;
typedef uint64_t aligned_u64 __attribute__((aligned(8)));
#define __aligned(n) __attribute__((aligned(n)))
#define BIT(n) (1u<<(n))
#define BIT_ULL(n) (1ull<<(n))
#define GENMASK(h,l) ((~0u<<(l))&(~0u>>(31-(h))))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof(*(a)))
#define DIV_ROUND_UP(n,d) (((n)+(d)-1)/(d))
#define ALIGN_POT(n,a) (((n)+(a)-1)&~((a)-1))
#define round_up ALIGN_POT
#define ALIGN ALIGN_POT
#define MAX2(a,b) ((a)>(b)?(a):(b))
#define WARN_ON(x) reference_warn(x)
static int reference_warn(bool condition) {return condition;}
#define WARN(x,...) ((void)(x))
#define UNREACHABLE(x) abort()
#define PVR_FEATURE_VALUE(d,f,out) (*(out)=feature_##f,0)
#define PVR_GET_FEATURE_VALUE(d,f,fallback) feature_##f
#define PVR_HAS_FEATURE(d,f) 1
#define feature_tile_size_x 16u
#define feature_tile_size_y 16u
#define feature_simple_parameter_format_version 2u
#define feature_isp_samples_per_pixel 1u
#include "pvr_rogue_fwif.h"
#include "pvr_rogue_cr_defs_client.h"
#define ROGUE_NUM_PC_ENTRIES_PER_PAGE 0x400u
#define ROGUE_NUM_PD_ENTRIES_PER_PAGE 0x200u
#define ROGUE_NUM_PT_ENTRIES_PER_PAGE 0x200u
static unsigned fls(unsigned v) { unsigned n=0; while(v){++n;v>>=1;}return n; }
static uint64_t util_next_power_of_two64(uint64_t x) {uint64_t n=1;while(n<x)n*=2;return n;}
struct pvr_device_info {unsigned unused;};
struct pvr_physical_device {struct pvr_device_info dev_info;};
struct pvr_device {struct pvr_physical_device *pdevice;};
struct pvr_file {struct pvr_device *pvr_dev;};
struct pvr_fw_object {u32 address;};
struct pvr_free_list {uint64_t size;struct pvr_fw_object *fw_obj;};
struct pvr_hwrt_dataset {struct rogue_fwif_hwrtdata_common common;struct pvr_fw_object *common_fw_obj;struct pvr_free_list *free_lists[2];};
struct pvr_hwrt_data {struct rogue_fwif_hwrtdata data;struct pvr_fw_object *fw_obj,*srtc_obj,*raa_obj;};
struct drm_pvr_create_hwrt_geom_data_args {u64 tpc_dev_addr,vheap_table_dev_addr,rtc_dev_addr;u32 tpc_size,tpc_stride;};
struct drm_pvr_create_hwrt_rt_data_args {u64 pm_mlist_dev_addr,macrotile_array_dev_addr,region_header_dev_addr;};
struct drm_pvr_ioctl_create_hwrt_dataset_args {
 u32 width,height,samples,layers,region_header_size;
 u32 isp_merge_lower_x,isp_merge_lower_y,isp_merge_upper_x,isp_merge_upper_y,isp_merge_scale_x,isp_merge_scale_y;
 struct drm_pvr_create_hwrt_geom_data_args geom_data_args;
};
struct pvr_rt_mtile_info {
 u32 mtile_x[3],mtile_y[3],tile_max_x,tile_max_y,tile_size_x,tile_size_y,num_tiles_x,num_tiles_y;
 u32 mtiles_x,mtiles_y,mtile_x1,mtile_y1,mtile_x2,mtile_y2,mtile_x3,mtile_y3,x_tile_max,y_tile_max,tiles_per_mtile_x,tiles_per_mtile_y;
};
static void rogue_get_num_macrotiles_xy(const struct pvr_device_info *d,u32 *x,u32 *y){(void)d;*x=4;*y=4;}
#define PVR_BO_FW_FLAGS_DEVICE_UNCACHED 0
#define SRTC_ENTRY_SIZE 4
#define RAA_ENTRY_SIZE 4
static u32 reference_address;
static int pvr_fw_object_create(struct pvr_device *d,size_t n,unsigned flags,void (*init)(void*,void*),void *arg,struct pvr_fw_object **out)
{(void)d;(void)flags;*out=malloc(sizeof(**out));assert(*out);(*out)->address=reference_address;void *p=calloc(1,n);assert(p);if(init)init(p,arg);free(p);return 0;}
static void pvr_fw_object_destroy(struct pvr_fw_object *p){free(p);}
static void pvr_fw_object_get_fw_addr(struct pvr_fw_object *p,u32 *out){*out=p->address;}
static void pvr_free_list_add_hwrt(struct pvr_free_list *list,struct pvr_hwrt_data *rt){assert(list&&rt);}
'''

def extract(text, name):
    match = re.search(r'(?:static (?:inline )?)?(?:void|int|uint32_t|uint64_t)\s*\n?' + name + r'\(', text)
    if not match:
        raise ValueError(name)
    start = text.index('{', match.start())
    end, depth = start + 1, 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[match.start():end] + '\n'

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sources', type=Path, required=True)
    parser.add_argument('--mesa', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--mesa-archive', type=Path, required=True)
    parser.add_argument('--compiler', default='clang-21')
    args = parser.parse_args()
    out = args.output.resolve(); out.mkdir(parents=True)
    manifest = json.loads((args.sources.parent / 'manifest.json').read_text())
    headers = ['pvr_rogue_fwif.h','pvr_rogue_defs.h','pvr_rogue_cr_defs.h','pvr_rogue_cr_defs_client.h',
               'pvr_rogue_fwif_common.h','pvr_rogue_fwif_shared.h','pvr_rogue_fwif_shared_check.h','pvr_rogue_fwif_check.h']
    inputs = []
    for name in headers + ['pvr_hwrt.c']:
        source = args.sources / name
        pin = [r for r in manifest if r['path'] == 'imagination/' + name]
        assert len(pin) == 1 and pin[0]['sha256'] == sha(source), name
        inputs.append(source)
        if name in headers:
            (out / name).write_text(re.sub(r'^#include <linux/[^>]+>\n', '', source.read_text(), flags=re.M))
    revision = '86c7176d2dd0065555521bf8c61f208bf53c75d6'
    assert sha(args.mesa_archive) == 'ca5e3ac4b61b8dc659cc489c7eb3c7bcc750f6820fbf39f0b831ac87fb00da58'
    mesa = args.mesa / 'src/imagination/vulkan/pvr_arch_job_render.c'
    definitions = args.mesa / 'src/imagination/include/hwdef/rogue_hw_defs.h'
    inputs += [mesa, definitions, args.mesa / 'src/imagination/include/hwdef/rogue_hw_utils.h',
               args.mesa / 'src/imagination/common/device_info/bxm-4-64.h']
    # Preserve exact constants from the reference, including page/entry sizes.
    constants = ['ROGUE_NUM_PM_ADDRESS_SPACES','ROGUE_MLIST_ENTRY_STRIDE','ROGUE_PM_MAX_PB_VIRT_ADDR_SPACE',
                 'ROGUE_TAIL_POINTER_SIZE','ROGUE_TE_TPC_CACHE_LINE_SIZE','ROGUE_BIF_PM_PHYSICAL_PAGE_SHIFT']
    reference = '/* Reference excerpts retain source copyright/license notices in inputs/. */\n' + SHIM
    for name in constants:
        reference += re.search(r'^#define ' + name + r' .+$', definitions.read_text(), re.M)[0] + '\n'
    reference += '#undef ROGUE_BIF_PM_PHYSICAL_PAGE_SIZE\n#define ROGUE_BIF_PM_PHYSICAL_PAGE_SIZE (1u << ROGUE_BIF_PM_PHYSICAL_PAGE_SHIFT)\n'
    xml_source = args.mesa / 'src/imagination/csbgen/rogue/cr.xml'
    inputs.append(xml_source)
    xml = ET.fromstring(xml_source.read_text())
    base = xml.find(".//struct[@name='TE_PSGREGION_ADDR']/field[@name='base']")
    unit = xml.find(".//field[@name='region_stride']/define[@name='UNIT_SIZE']")
    reference += '#define ROGUE_CR_TE_PSGREGION_ADDR_BASE_ALIGNMENT ' + str(1 << int(base.attrib['shift'])) + '\n'
    reference += '#define ROGUE_CR_TE_PSG_REGION_STRIDE_UNIT_SIZE ' + unit.attrib['value'] + '\n'
    profile = (args.mesa / 'src/imagination/common/device_info/bxm-4-64.h').read_text()
    profile = profile[profile.index('static const struct pvr_device_features pvr_device_features_36_V_104_183'):]
    for feature, value in [('tile_size_x',16),('tile_size_y',16),('isp_samples_per_pixel',1),('simple_parameter_format_version',2)]:
        assert int(re.search(r'\.' + feature + r' = ([0-9]+)U',profile)[1]) == value
    assert '.has_screen_size8K = true' in profile and '.has_roguexe = true' in profile
    helpers = (args.mesa / 'src/imagination/include/hwdef/rogue_hw_utils.h').read_text()
    reference += extract(helpers,'rogue_get_region_header_size')
    reference += extract(helpers,'rogue_get_render_size_max')
    board = (args.sources / 'pvr_hwrt.c').read_text()
    for name in ['get_cr_isp_mtile_size_val','get_cr_multisamplectl_val','get_cr_te_aa_val','hwrtdata_common_init',
                 'hwrt_init_common_fw_structure','hwrt_fw_data_init','hwrt_data_init_fw_structure']:
        reference += extract(board, name)
    for name in ['pvr_get_samples_in_xy','pvr_arch_rt_mtile_info_init','pvr_rt_get_isp_region_size',
                 'pvr_rt_get_tail_ptr_stride_size','pvr_rt_get_mlist_size','pvr_rt_get_region_headers_stride_size']:
        reference += extract(mesa.read_text(), name)
    (out / 'reference.h').write_text(reference)
    inputs += [ROOT / p for p in ['drivers/gpu/pvr_render_target.c','include/pvr_render_target.h',
                                  'checks/pvr-render-target.c','checks/pvr-render-target.py']]
    with tarfile.open(args.mesa_archive, 'r:gz') as archive:
        for source in inputs:
            if source.is_relative_to(args.mesa):
                entry = 'mesa-' + revision + '/' + str(source.relative_to(args.mesa))
                assert archive.extractfile(entry).read() == source.read_bytes(), source
    snapshots = {}
    for i, p in enumerate(inputs):
        dst = out / 'inputs' / str(i) / p.name; dst.parent.mkdir(parents=True); shutil.copy2(p, dst)
        snapshots[str(p)] = {'sha256': sha(p), 'snapshot': str(dst.relative_to(out))}
    command = [args.compiler, '-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-parameter',
               '-Wno-unused-variable','-Wno-sign-compare','-fsanitize=address,undefined','-fno-omit-frame-pointer',
               '-I'+str(out),'-I'+str(ROOT/'include'),str(ROOT/'checks/pvr-render-target.c'),
               str(ROOT/'drivers/gpu/pvr_render_target.c'),'-o',str(out/'check')]
    (out/'compile-command.json').write_text(json.dumps(command,indent=2)+'\n')
    compile_result = subprocess.run(command,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
    (out/'compile.log').write_text(compile_result.stdout)
    compile_result.check_returncode()
    run = subprocess.run([str(out/'check')],text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=10)
    (out/'check.log').write_text(run.stdout)
    result = {'passed':run.returncode==0,'inputs':snapshots,'binary_sha256':sha(out/'check'),
        'reference_sha256':sha(out/'reference.h'),'mesa_revision':revision,
        'compiler':subprocess.check_output([args.compiler,'--version'],text=True),
        'physical_access':False,'firmware_execution':False}
    (out/'results.json').write_text(json.dumps(result,indent=2)+'\n')
    print(run.stdout,end=''); run.check_returncode()
if __name__ == '__main__':
    main()
