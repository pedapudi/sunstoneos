#!/usr/bin/env python3
"""Select pinned standalone HEVC syntax code with bounded fixed metadata."""
import argparse,hashlib,json,re
from pathlib import Path

def generate(source,reader,out):
    source=Path(source);reader=Path(reader);out=Path(out)
    pins=json.loads((Path(__file__).parent/'sources.json').read_text())
    rpins=json.loads((reader/'pins.json').read_text())
    def read(root,name,sha):
        b=(root/name).read_bytes();assert hashlib.sha256(b).hexdigest()==sha,name
        return b.decode()
    def hevc(name):return read(source,name,pins[name]['sha256'])
    def nal(name):return read(reader,name,rpins['files'][name]['sha256'])
    out.mkdir(parents=True,exist_ok=True)
    h=re.sub(r'^#include.*\n','',hevc('gsth265parser.h'),flags=re.M)
    names={'gst_h265_parse_vps','gst_h265_parser_parse_vps','gst_h265_parse_sps','gst_h265_parser_parse_sps','gst_h265_parse_pps','gst_h265_parser_parse_pps','gst_h265_parser_parse_slice_hdr','gst_h265_slice_hdr_free'}
    declarations=[b for b in re.findall(r'GST_CODEC_PARSERS_API\n[^;]+;',h) if any(re.search(r'\b'+n+r'\s*\(',b) for n in names)]
    assert len(declarations)==len(names)
    h=h[:h.index('GST_CODEC_PARSERS_API')]+'\n'.join(declarations)+'\nG_END_DECLS\n#endif\n'
    (out/'gst-hevc-types.h').write_text('#include "gst-port.h"\n'+h)
    n=nal('nalutils.h');n=n[n.index('typedef struct'):n.index('G_GNUC_INTERNAL\ngint scan_for_start_codes')]
    n=re.sub(r'typedef struct\n\{\n  GstBitWriter.*?\} NalWriter;','',n,flags=re.S)
    (out/'gst-nal.h').write_text('#include "gst-port.h"\n'+n)
    n=nal('nalutils.c');n=n[:n.index('\ngint\nscan_for_start_codes')]
    n=n.replace('#include "nalutils.h"','#include "gst-nal.h"').replace('(1 << i) - 1 + value','(UINT32_C(1) << i) - 1 + value').replace('    i++;','    if (++i > 30) return FALSE;')
    (out/'gst-nal.c').write_text(n)
    regs=hevc('cedrus_regs.h')
    notices=regs[:regs.index('#ifndef')]
    selected=regs[regs.index('#define VE_DEC_H265_DEC_NAL_HDR'):regs.index('#define VE_H264_SPS')]
    (out/'cedrus-hevc-regs.h').write_text(notices+'\n#include <stdint.h>\n#define BIT(n) (UINT32_C(1)<<(n))\n#define GENMASK(h,l) ((UINT32_MAX>>(31-(h))) & (UINT32_MAX<<(l)))\n#define SHIFT_AND_MASK_BITS(v,h,l) (((uint32_t)(v)<<(l))&GENMASK(h,l))\n#define VE_ENGINE_DEC_H265 0x500u\n'+selected)
    p=hevc('gsth265parser.c')
    def block(a,b):return p[p.index(a):p.index(b)]
    chunks=[p[:p.index('/**\n * SECTION:')], '#include "gst-hevc-types.h"\n#include "gst-nal.h"\n',
        block('/**** Default scaling_lists','static gboolean\ngst_h265_parse_nalu_header'),
        block('static gboolean\ngst_h265_parse_profile_tier_level','static GstH265ParserResult\ngst_h265_parser_parse_buffering_period'),
        block('GstH265ParserResult\ngst_h265_parser_parse_vps (','static gboolean\nnal_reader_has_more_data_in_payload')]
    s='\n'.join(chunks)
    # Reject unsupported geometry/extensions before upstream arithmetic or heap use.
    changes={
        'READ_UE_ALLOWED (&nr, sps->pic_width_in_luma_samples, 1, 16888);':'READ_UE_ALLOWED (&nr, sps->pic_width_in_luma_samples, 640, 640);',
        'READ_UE_ALLOWED (&nr, sps->pic_height_in_luma_samples, 1, 16888);':'READ_UE_ALLOWED (&nr, sps->pic_height_in_luma_samples, 480, 480);',
        'READ_UINT8 (&nr, sps->conformance_window_flag, 1);':'READ_UINT8 (&nr, sps->conformance_window_flag, 1);\n  if (sps->conformance_window_flag) goto error;',
        'READ_UE_MAX (&nr, sps->bit_depth_luma_minus8, 6);':'READ_UE_MAX (&nr, sps->bit_depth_luma_minus8, 0);',
        'READ_UE_MAX (&nr, sps->bit_depth_chroma_minus8, 6);':'READ_UE_MAX (&nr, sps->bit_depth_chroma_minus8, 0);',
        'READ_UINT8 (&nr, pps->tiles_enabled_flag, 1);':'READ_UINT8 (&nr, pps->tiles_enabled_flag, 1);\n  if (pps->tiles_enabled_flag) goto error;',
        'READ_UINT8 (&nr, pps->entropy_coding_sync_enabled_flag, 1);':'READ_UINT8 (&nr, pps->entropy_coding_sync_enabled_flag, 1);\n  if (pps->entropy_coding_sync_enabled_flag) goto error;',
    }
    for old,new in changes.items():assert s.count(old)==1,old;s=s.replace(old,new)
    a=s.index('  if (pps->tiles_enabled_flag || pps->entropy_coding_sync_enabled_flag) {',s.index('gst_h265_parser_parse_slice_hdr ('))
    b=s.index('  if (pps->slice_segment_header_extension_present_flag) {',a)
    s=s[:a]+'  if (pps->tiles_enabled_flag || pps->entropy_coding_sync_enabled_flag) goto error;\n\n'+s[b:]
    # Cedrus consumes the physical offset before alignment_one_bit; retain that
    # offset and validate rather than silently skipping the alignment syntax.
    a=s.index('  /* Skip the byte alignment bits */');b=s.index('  return GST_H265_PARSER_OK;',a)
    s=s[:a]+'''  slice->header_size = nal_reader_get_pos (&nr);
  slice->n_emulation_prevention_bytes = nal_reader_get_epb_count (&nr);
  { guint8 bit; READ_UINT8 (&nr, bit, 1); if (bit != 1) goto error;
    while (!nal_reader_is_byte_aligned (&nr)) {
      READ_UINT8 (&nr, bit, 1); if (bit != 0) goto error;
    }
  }

'''+s[b:]
    s+='\nvoid gst_h265_slice_hdr_free (GstH265SliceHdr *s) { s->entry_point_offset_minus1 = NULL; s->num_entry_point_offsets = 0; }\n'
    (out/'gst-hevc-parser.c').write_text(s)
    (out/'gst-inputs.json').write_text(json.dumps({'hevc':pins,'reader':rpins},indent=2)+'\n')
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--source',required=True);p.add_argument('--reader',required=True);p.add_argument('--output',required=True);a=p.parse_args();generate(a.source,a.reader,a.output)
