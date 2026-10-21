/* SPDX-License-Identifier: LGPL-2.0-or-later */
/* Standalone GStreamer syntax adapter; register fields follow Linux Cedrus
 * cedrus_h265.c, pinned in apps/hevc/sources.json. */
#include "a733_video_hevc.h"
#include "gst-hevc-types.h"
#include "cedrus-hevc-regs.h"
#include <string.h>
struct workspace {
 GstH265Parser parser;GstH265VPS vps;GstH265SPS sps;GstH265PPS pps;
 GstH265SliceHdr slice;struct a733_hevc_plan plan;
};
_Static_assert(sizeof(struct workspace)<=A733_HEVC_PARSER_BYTES,"HEVC parser workspace bound");
size_t a733_hevc_parser_bytes(void){return sizeof(struct workspace);}
static unsigned prefix(const uint8_t *p,size_t n)
{if(n>=4&&!p[0]&&!p[1]&&!p[2]&&p[3]==1)return 4;
 return n>=3&&!p[0]&&!p[1]&&p[2]==1?3:0;}
/* The GStreamer reader drops any 0x000003 without checking what follows, and
 * the hardware removes the same bytes from the slice data. Require every
 * two-zero prefix in the whole unit to start a valid emulation-prevention
 * sequence, and a nonzero final byte, so both readers see identical RBSP. */
static int escaped(const uint8_t *p,size_t n)
{
 unsigned zeros=0;
 for(size_t i=0;i<n;++i){
  if(zeros>=2){if(p[i]<3||(p[i]==3&&i+1<n&&p[i+1]>3))return 0;if(p[i]==3){zeros=0;continue;}}
  zeros=p[i]?0:zeros+1;
 }
 return n&&p[n-1];
}
static int sps_valid(const GstH265SPS *s)
{return s->profile_tier_level.profile_idc==1&&!s->profile_tier_level.interlaced_source_flag&&
 !s->max_sub_layers_minus1&&s->chroma_format_idc==1&&!s->separate_colour_plane_flag&&
 s->pic_width_in_luma_samples==640&&s->pic_height_in_luma_samples==480&&
 !s->conformance_window_flag&&!s->bit_depth_luma_minus8&&!s->bit_depth_chroma_minus8&&
 s->log2_min_luma_coding_block_size_minus3+s->log2_diff_max_min_luma_coding_block_size==3&&
 !s->scaling_list_enabled_flag&&!s->pcm_enabled_flag&&!s->long_term_ref_pics_present_flag&&
 !s->sps_extension_flag&&!s->vui_params.field_seq_flag;}
static int pps_valid(const GstH265PPS *p)
{return !p->dependent_slice_segments_enabled_flag&&!p->tiles_enabled_flag&&
 !p->entropy_coding_sync_enabled_flag&&!p->weighted_pred_flag&&!p->weighted_bipred_flag&&
 !p->lists_modification_present_flag&&!p->scaling_list_data_present_flag&&!p->pps_extension_flag&&
 !p->slice_segment_header_extension_present_flag;}
#define FLAG(v,bit) ((v)?(bit):0)
/* Field packing of cedrus_h265_setup. Refused features contribute zeros. */
static void registers(struct a733_hevc_picture *o,const GstH265SPS *s,
 const GstH265PPS *p,const GstH265SliceHdr *h,unsigned type)
{
 o->nal_type=type;o->type=h->type;o->poc=h->pic_order_cnt_lsb;
 o->sps=VE_DEC_H265_DEC_SPS_HDR_MAX_TRANSFORM_HIERARCHY_DEPTH_INTRA(s->max_transform_hierarchy_depth_intra)|
 VE_DEC_H265_DEC_SPS_HDR_MAX_TRANSFORM_HIERARCHY_DEPTH_INTER(s->max_transform_hierarchy_depth_inter)|
 VE_DEC_H265_DEC_SPS_HDR_LOG2_DIFF_MAX_MIN_TRANSFORM_BLOCK_SIZE(s->log2_diff_max_min_transform_block_size)|
 VE_DEC_H265_DEC_SPS_HDR_LOG2_MIN_TRANSFORM_BLOCK_SIZE_MINUS2(s->log2_min_transform_block_size_minus2)|
 VE_DEC_H265_DEC_SPS_HDR_LOG2_DIFF_MAX_MIN_LUMA_CODING_BLOCK_SIZE(s->log2_diff_max_min_luma_coding_block_size)|
 VE_DEC_H265_DEC_SPS_HDR_LOG2_MIN_LUMA_CODING_BLOCK_SIZE_MINUS3(s->log2_min_luma_coding_block_size_minus3)|
 VE_DEC_H265_DEC_SPS_HDR_BIT_DEPTH_CHROMA_MINUS8(s->bit_depth_chroma_minus8)|
 VE_DEC_H265_DEC_SPS_HDR_BIT_DEPTH_LUMA_MINUS8(s->bit_depth_luma_minus8)|
 VE_DEC_H265_DEC_SPS_HDR_CHROMA_FORMAT_IDC(s->chroma_format_idc)|
 FLAG(s->strong_intra_smoothing_enabled_flag,VE_DEC_H265_DEC_SPS_HDR_FLAG_STRONG_INTRA_SMOOTHING_ENABLE)|
 FLAG(s->temporal_mvp_enabled_flag,VE_DEC_H265_DEC_SPS_HDR_FLAG_SPS_TEMPORAL_MVP_ENABLED)|
 FLAG(s->sample_adaptive_offset_enabled_flag,VE_DEC_H265_DEC_SPS_HDR_FLAG_SAMPLE_ADAPTIVE_OFFSET_ENABLED)|
 FLAG(s->amp_enabled_flag,VE_DEC_H265_DEC_SPS_HDR_FLAG_AMP_ENABLED);
 /* PCM is refused; GStreamer leaves its unsignaled fields zero. */
 o->pcm=VE_DEC_H265_DEC_PCM_CTRL_PCM_SAMPLE_BIT_DEPTH_CHROMA_MINUS1(s->pcm_sample_bit_depth_chroma_minus1)|
 VE_DEC_H265_DEC_PCM_CTRL_PCM_SAMPLE_BIT_DEPTH_LUMA_MINUS1(s->pcm_sample_bit_depth_luma_minus1);
 o->pps0=VE_DEC_H265_DEC_PPS_CTRL0_PPS_CR_QP_OFFSET(p->cr_qp_offset)|
 VE_DEC_H265_DEC_PPS_CTRL0_PPS_CB_QP_OFFSET(p->cb_qp_offset)|
 VE_DEC_H265_DEC_PPS_CTRL0_INIT_QP_MINUS26(p->init_qp_minus26)|
 VE_DEC_H265_DEC_PPS_CTRL0_DIFF_CU_QP_DELTA_DEPTH(p->diff_cu_qp_delta_depth)|
 FLAG(p->cu_qp_delta_enabled_flag,VE_DEC_H265_DEC_PPS_CTRL0_FLAG_CU_QP_DELTA_ENABLED)|
 FLAG(p->transform_skip_enabled_flag,VE_DEC_H265_DEC_PPS_CTRL0_FLAG_TRANSFORM_SKIP_ENABLED)|
 FLAG(p->constrained_intra_pred_flag,VE_DEC_H265_DEC_PPS_CTRL0_FLAG_CONSTRAINED_INTRA_PRED)|
 FLAG(p->sign_data_hiding_enabled_flag,VE_DEC_H265_DEC_PPS_CTRL0_FLAG_SIGN_DATA_HIDING_ENABLED);
 o->pps1=VE_DEC_H265_DEC_PPS_CTRL1_LOG2_PARALLEL_MERGE_LEVEL_MINUS2(p->log2_parallel_merge_level_minus2)|
 FLAG(p->loop_filter_across_slices_enabled_flag,VE_DEC_H265_DEC_PPS_CTRL1_FLAG_PPS_LOOP_FILTER_ACROSS_SLICES_ENABLED)|
 FLAG(p->loop_filter_across_tiles_enabled_flag,VE_DEC_H265_DEC_PPS_CTRL1_FLAG_LOOP_FILTER_ACROSS_TILES_ENABLED)|
 FLAG(p->transquant_bypass_enabled_flag,VE_DEC_H265_DEC_PPS_CTRL1_FLAG_TRANSQUANT_BYPASS_ENABLED);
 /* One frame picture, at most one list0 reference, collocated index zero. */
 o->slice0=VE_DEC_H265_DEC_SLICE_HDR_INFO0_FIVE_MINUS_MAX_NUM_MERGE_CAND(h->five_minus_max_num_merge_cand)|
 VE_DEC_H265_DEC_SLICE_HDR_INFO0_SLICE_TYPE(h->type)|
 FLAG(h->collocated_from_l0_flag,VE_DEC_H265_DEC_SLICE_HDR_INFO0_FLAG_COLLOCATED_FROM_L0)|
 FLAG(h->cabac_init_flag,VE_DEC_H265_DEC_SLICE_HDR_INFO0_FLAG_CABAC_INIT)|
 FLAG(h->sao_chroma_flag,VE_DEC_H265_DEC_SLICE_HDR_INFO0_FLAG_SLICE_SAO_CHROMA)|
 FLAG(h->sao_luma_flag,VE_DEC_H265_DEC_SLICE_HDR_INFO0_FLAG_SLICE_SAO_LUMA)|
 FLAG(h->temporal_mvp_enabled_flag,VE_DEC_H265_DEC_SLICE_HDR_INFO0_FLAG_SLICE_TEMPORAL_MVP_ENABLE)|
 VE_DEC_H265_DEC_SLICE_HDR_INFO0_FLAG_FIRST_SLICE_SEGMENT_IN_PIC;
 /* Cedrus and CedarC set bit 21 for a P or B slice whose references all
  * precede it in output order. The only admitted reference is the IDR at
  * POC 0, so every admitted P picture sets it. */
 o->slice1=VE_DEC_H265_DEC_SLICE_HDR_INFO1_SLICE_TC_OFFSET_DIV2(h->tc_offset_div2)|
 VE_DEC_H265_DEC_SLICE_HDR_INFO1_SLICE_BETA_OFFSET_DIV2(h->beta_offset_div2)|
 VE_DEC_H265_DEC_SLICE_HDR_INFO1_SLICE_CR_QP_OFFSET(h->cr_qp_offset)|
 VE_DEC_H265_DEC_SLICE_HDR_INFO1_SLICE_CB_QP_OFFSET(h->cb_qp_offset)|
 VE_DEC_H265_DEC_SLICE_HDR_INFO1_SLICE_QP_DELTA(h->qp_delta)|
 FLAG(h->deblocking_filter_disabled_flag,VE_DEC_H265_DEC_SLICE_HDR_INFO1_FLAG_SLICE_DEBLOCKING_FILTER_DISABLED)|
 FLAG(h->loop_filter_across_slices_enabled_flag,VE_DEC_H265_DEC_SLICE_HDR_INFO1_FLAG_SLICE_LOOP_FILTER_ACROSS_SLICES_ENABLED)|
 FLAG(h->type==GST_H265_P_SLICE,VE_DEC_H265_DEC_SLICE_HDR_INFO1_FLAG_SLICE_NOT_LOW_DELAY);
 o->slice2=0;
}
int a733_hevc_parse(const uint8_t *b,size_t n,const uint64_t r[4],void *memory,size_t bytes,struct a733_hevc_plan *out)
{
 if(!b||!r||!out||!memory||bytes<sizeof(struct workspace)||(uintptr_t)memory%_Alignof(struct workspace)||
 !n||n>524288||r[0]||!r[1]||r[1]>262144||r[2]!=r[1]||!r[3]||r[3]>262144||r[2]+r[3]!=n)return 0;
 struct workspace *w=memory;memset(w,0,sizeof(*w));size_t at=0;unsigned stage=0;
 while(at<n){
  unsigned pre=prefix(b+at,n-at);if(!pre)return 0;size_t data=at+pre,end=data;
  while(end<n&&!prefix(b+end,n-end))++end;
  if(end-data<3||end-data>262144||b[data]&0x80||((b[data]&1)<<5)|(b[data+1]>>3)||!escaped(b+data,end-data))return 0;
  unsigned type=(b[data]>>1)&63;if((b[data+1]&7)!=1)return 0;
  GstH265NalUnit nal={.type=type,.temporal_id_plus1=1,.size=(guint)(end-data),.offset=(guint)data,
   .sc_offset=(guint)at,.valid=1,.data=(guint8 *)(uintptr_t)b,.header_bytes=2};
  if(stage==0){if(type!=32||gst_h265_parser_parse_vps(&w->parser,&nal,&w->vps)!=GST_H265_PARSER_OK||w->vps.max_layers_minus1||w->vps.max_sub_layers_minus1)return 0;}
  else if(stage==1){if(type!=33||gst_h265_parser_parse_sps(&w->parser,&nal,&w->sps,TRUE)!=GST_H265_PARSER_OK||!sps_valid(&w->sps))return 0;}
  else if(stage==2){if(type!=34||gst_h265_parser_parse_pps(&w->parser,&nal,&w->pps)!=GST_H265_PARSER_OK||!pps_valid(&w->pps))return 0;}
  else if(stage<5){
   unsigned i=stage-3;if((!i&&type!=19&&type!=20)||(i&&type!=1)||
    (i?at!=r[2]:end!=r[1])||gst_h265_parser_parse_slice_hdr(&w->parser,&nal,&w->slice)!=GST_H265_PARSER_OK)return 0;
   GstH265SliceHdr *h=&w->slice;GstH265PPS *p=h->pps;GstH265SPS *s=p?p->sps:NULL;
   if(!p||!s||!sps_valid(s)||!pps_valid(p)||!h->first_slice_segment_in_pic_flag||h->dependent_slice_segment_flag||
    h->segment_address||!h->pic_output_flag||h->type!=(i?GST_H265_P_SLICE:GST_H265_I_SLICE)||h->num_ref_idx_l0_active_minus1||
    h->num_ref_idx_l1_active_minus1||h->num_long_term_sps||h->num_long_term_pics||h->collocated_ref_idx||
    h->num_entry_point_offsets||h->NumPocTotalCurr!=(i?1:0))return 0;
   /* A POC lsb below half its range keeps PicOrderCntMsb zero after the IDR. */
   if(i){GstH265ShortTermRefPicSet *ref=h->short_term_ref_pic_set_sps_flag?&s->short_term_ref_pic_set[h->short_term_ref_pic_set_idx]:&h->short_term_ref_pic_sets;
    if(!h->pic_order_cnt_lsb||h->pic_order_cnt_lsb>32767||h->pic_order_cnt_lsb>=(1u<<(s->log2_max_pic_order_cnt_lsb_minus4+3))||ref->NumNegativePics!=1||ref->NumPositivePics||
     !ref->UsedByCurrPicS0[0]||ref->DeltaPocS0[0]!=-(int32_t)h->pic_order_cnt_lsb)return 0;}
   else if(h->pic_order_cnt_lsb)return 0;
   struct a733_hevc_picture *o=&w->plan.pictures[i];o->offset=(uint32_t)data;o->bytes=(uint32_t)(end-data);
   /* header_size ends before alignment_bit_equal_to_one and counts escaped
    * bytes; the hardware counts unescaped bits from the NAL header. */
   if(h->header_size<h->n_emulation_prevention_bytes*8||h->header_size+16>=o->bytes*8)return 0;
   o->header_bits=16+h->header_size-8*h->n_emulation_prevention_bytes;
   registers(o,s,p,h,type);
  }else return 0;
  ++stage;at=end;
 }
 if(stage!=5)return 0;*out=w->plan;return 1;
}
