#!/usr/bin/env python3
"""Select the pinned standalone upstream reader/header parser; no framework runtime."""
import argparse,hashlib,json,re
from pathlib import Path
REV='d31ce8e5e1aacf8f5e5beabb5c81ce2e4da5c202'
def generate(source,out):
    source=Path(source);out=Path(out);out.mkdir(parents=True,exist_ok=True)
    pins=json.loads((source/'pins.json').read_text());assert pins['revision']==REV
    def read(name):
        data=(source/name).read_bytes();assert hashlib.sha256(data).hexdigest()==pins['files'][name]['sha256'];return data.decode()
    header=read('gsth264parser.h');header=re.sub(r'^#include.*\n','',header,flags=re.M)
    header=header[:header.index('GST_CODEC_PARSERS_API')]+ '\n#endif\n'
    # Keep declarations/structs exactly; private runtime supplies only fixed metadata.
    (out/'gst-h264-types.h').write_text('#include "gst-port.h"\n'+header)
    n=read('nalutils.h');n=n[n.index('typedef struct'):n.index('G_GNUC_INTERNAL\ngint scan_for_start_codes')]
    n=re.sub(r'typedef struct\n\{\n  GstBitWriter.*?\} NalWriter;','',n,flags=re.S)
    (out/'gst-nal.h').write_text('#include "gst-port.h"\n'+n)
    reader=read('nalutils.c');reader=reader[:reader.index('\ngint\nscan_for_start_codes')]
    reader=reader.replace('#include "nalutils.h"','#include "gst-nal.h"').replace('(1 << i) - 1 + value','(UINT32_C(1) << i) - 1 + value')
    # Restrict pathological Golomb loops and signed conversion before overflow.
    reader=reader.replace('    i++;','    if (++i > 30) return FALSE;')
    (out/'gst-nal.c').write_text(reader)
    p=read('gsth264parser.c')
    def block(start,end):return p[p.index(start):p.index(end)]
    parts=[p[:p.index('/**\n * SECTION:')], '#include "gst-h264-types.h"\n#include "gst-nal.h"\n',block('/**** Default scaling_lists','static gboolean\ngst_h264_parse_nalu_header'),block('static gboolean\ngst_h264_parse_hrd_parameters','static GstH264ParserResult\ngst_h264_parser_parse_buffering_period'),block('static gboolean\ngst_h264_parse_sps_data','/* Parse subset_seq_parameter_set()'),block('GstH264ParserResult\ngst_h264_parse_sps (','/**\n * gst_h264_parser_parse_subset_sps:'),block('GstH264ParserResult\ngst_h264_parse_pps (','/**\n * gst_h264_parser_parse_pps:'),block('GstH264ParserResult\ngst_h264_parser_parse_slice_hdr (','static void\ngst_h264_sps_mvc_clear'),block('void\ngst_h264_quant_matrix_8x8_get_raster_from_zigzag','/**\n * gst_h264_quant_matrix_4x4_get_zigzag_from_raster'),block('void\ngst_h264_quant_matrix_4x4_get_raster_from_zigzag','/**\n * gst_h264_video_calculate_framerate')]
    result='\n'.join(parts)
    # Explicit admitted bounds before upstream arithmetic/allocation paths.
    changes={
      'READ_UE (nr, sps->num_ref_frames);':'READ_UE_MAX (nr, sps->num_ref_frames, 4);',
      'READ_UE (nr, sps->pic_width_in_mbs_minus1);':'READ_UE_MAX (nr, sps->pic_width_in_mbs_minus1, 119);',
      'READ_UE (nr, sps->pic_height_in_map_units_minus1);':'READ_UE_MAX (nr, sps->pic_height_in_map_units_minus1, 67);',
      'READ_UINT8 (nr, sps->frame_mbs_only_flag, 1);':'READ_UINT8 (nr, sps->frame_mbs_only_flag, 1);\n  if (!sps->frame_mbs_only_flag) goto error;',
      'READ_UE (nr, sps->frame_crop_left_offset);':'READ_UE_MAX (nr, sps->frame_crop_left_offset, 959);',
      'READ_UE (nr, sps->frame_crop_right_offset);':'READ_UE_MAX (nr, sps->frame_crop_right_offset, 959);',
      'READ_UE (nr, sps->frame_crop_top_offset);':'READ_UE_MAX (nr, sps->frame_crop_top_offset, 543);',
      'READ_UE (nr, sps->frame_crop_bottom_offset);':'READ_UE_MAX (nr, sps->frame_crop_bottom_offset, 543);',
      'if (pps->num_slice_groups_minus1 > 0) {':'if (pps->num_slice_groups_minus1 > 0) goto error;\n  if (pps->num_slice_groups_minus1 > 0) {',
    }
    for old,new in changes.items():assert result.count(old)==1,old;result=result.replace(old,new)
    start=result.index('  if (pps->num_slice_groups_minus1 > 0) {')
    end=result.index('  READ_UE_MAX (&nr, pps->num_ref_idx_l0_active_minus1',start)
    result=result[:start]+result[end:]
    result=result.replace('gboolean parse_pred_weight_table, gboolean parse_dec_ref_pic_marking)\n{','gboolean parse_pred_weight_table, gboolean parse_dec_ref_pic_marking)\n{\n  (void)parse_pred_weight_table; (void)parse_dec_ref_pic_marking;')
    # Unreachable FMO allocation is removed, not replaced by a heap shim.
    result=result.replace('g_new (guint8, pps->pic_size_in_map_units_minus1 + 1)','NULL')
    result=result.replace('gst_h264_pps_clear (pps);','pps->slice_group_id = NULL;')
    (out/'gst-h264-parser.c').write_text(result)
    (out/'gst-inputs.json').write_text(json.dumps(pins,indent=2)+'\n')
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--source',required=True);p.add_argument('--output',required=True);a=p.parse_args();generate(a.source,a.output)
