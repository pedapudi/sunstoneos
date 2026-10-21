/* SPDX-License-Identifier: LGPL-2.0-or-later
 * Header parser: GStreamer d31ce8e5e1aacf8f5e5beabb5c81ce2e4da5c202,
 * upstream selected by gst-source.py with original notices preserved.
 * POC/list/MMCO adaptation from that revision's gsth264decoder.c and
 * gsth264picture.c. Copyright (C) 2019 Seungha Yang and GStreamer contributors.
 * GNU Library General Public License version 2 or (at your option) later;
 * WITHOUT ANY WARRANTY, including MERCHANTABILITY or FITNESS FOR A PURPOSE.
 * See apps/h264/sources.json and cached upstream COPYING for complete notices.
 */
#include "a733_h264_high.h"
#include "gst-h264-types.h"
#include <string.h>
GstH264ParserResult gst_h264_parse_sps(GstH264NalUnit *,GstH264SPS *);
GstH264ParserResult gst_h264_parse_pps(GstH264NalParser *,GstH264NalUnit *,GstH264PPS *);
GstH264ParserResult gst_h264_parser_parse_slice_hdr(GstH264NalParser *,GstH264NalUnit *,GstH264SliceHdr *,gboolean,gboolean);
void gst_h264_quant_matrix_8x8_get_raster_from_zigzag(guint8 *,const guint8 *);
void gst_h264_quant_matrix_4x4_get_raster_from_zigzag(guint8 *,const guint8 *);
struct ref { int32_t poc,top,bottom,pic_num;uint32_t frame,slot; };
struct dpb {struct ref refs[4];unsigned count,prev_frame,prev_ref_frame;int32_t prev_msb,prev_lsb;uint32_t frame_offset,started,gop;};
struct h264_high {
    GstH264NalParser parser;
    GstH264SliceHdr slice;
    struct dpb committed,pending;
    struct h264_high_plan prepared;
    unsigned pending_valid;
};
_Static_assert(sizeof(struct h264_high)<=H264_HIGH_METADATA_BYTES,"High metadata bound");
size_t h264_high_bytes(void){return sizeof(struct h264_high);}
struct h264_high *h264_high_init(void *p,size_t n)
{if(!p||n<sizeof(struct h264_high)||(uintptr_t)p%16)return NULL;memset(p,0,sizeof(struct h264_high));return p;}
static size_t prefix(const uint8_t *p,size_t n,size_t a)
{if(n-a>=4&&!p[a]&&!p[a+1]&&!p[a+2]&&p[a+3]==1)return 4;if(n-a>=3&&!p[a]&&!p[a+1]&&p[a+2]==1)return 3;return 0;}
static uint32_t page(uint32_t n){return (n+4095)&~4095u;}
static int sort_before(struct ref a,struct ref b,int kind,int32_t poc)
{
    if(kind==0)return a.pic_num>b.pic_num;
    int aa=a.poc<poc,bb=b.poc<poc;
    if(aa!=bb)return kind==1?aa:!aa;
    return aa?(a.poc>b.poc):(a.poc<b.poc);
}
/* Adapted frame P/B construction, including identical B-list1 swap,
 * gsth264decoder.c:2650..2971. Array length is bounded before sorting. */
static void defaults(const struct dpb *d,unsigned type,int32_t poc,struct ref list[2][8],unsigned count[2])
{
    count[0]=type==2?0:d->count;count[1]=type==1?d->count:0;
    for(unsigned l=0;l<2;++l){
        for(unsigned i=0;i<count[l];++i)list[l][i]=d->refs[i];
        for(unsigned i=1;i<count[l];++i){struct ref x=list[l][i];unsigned j=i;
            while(j&&sort_before(x,list[l][j-1],type==0?0:l+1,poc)){list[l][j]=list[l][j-1];--j;}list[l][j]=x;}
    }
    if(type==1&&count[0]>1&&!memcmp(list[0],list[1],count[0]*sizeof(struct ref))){struct ref x=list[1][0];list[1][0]=list[1][1];list[1][1]=x;}
}
/* Short-term modification process, adapted modify_ref_pic_list:3161..3326.
 * Missing references and overlong modifications fail; no concealment. */
static int modify(const struct dpb *d,const GstH264SliceHdr *s,unsigned l,
    uint32_t max_frame,struct ref list[8],unsigned active)
{
    unsigned n=l?s->n_ref_pic_list_modification_l1:s->n_ref_pic_list_modification_l0;
    const GstH264RefPicListModification *m=l?s->ref_pic_list_modification_l1:s->ref_pic_list_modification_l0;
    int32_t prediction=(int32_t)s->frame_num;
    unsigned index=0;
    if(n>32||active>4)return 0;
    for(unsigned i=0;i<n;++i){
        if(m[i].modification_of_pic_nums_idc==3)break;
        if(m[i].modification_of_pic_nums_idc>1||index>=active||m[i].value.abs_diff_pic_num_minus1>=max_frame)return 0;
        int32_t delta=(int32_t)m[i].value.abs_diff_pic_num_minus1+1;
        prediction+=m[i].modification_of_pic_nums_idc==0?-delta:delta;
        if(prediction<0)prediction+=(int32_t)max_frame;if(prediction>=(int32_t)max_frame)prediction-=(int32_t)max_frame;
        int32_t number=prediction>(int32_t)s->frame_num?prediction-(int32_t)max_frame:prediction;
        const struct ref *found=NULL;for(unsigned j=0;j<d->count;++j)if(d->refs[j].pic_num==number)found=&d->refs[j];
        if(!found)return 0;
        for(unsigned j=active;j>index;--j)list[j]=list[j-1];list[index++]=*found;
        unsigned out=index;for(unsigned j=index;j<=active;++j)if(list[j].pic_num!=number)list[out++]=list[j];
    }
    return 1;
}
static int marking(struct dpb *d,const GstH264SliceHdr *s,unsigned idr,unsigned ref,unsigned slot,int32_t top,int32_t bottom,unsigned limit,unsigned *reset)
{
    *reset=0;
    if(!ref)return 1;
    const GstH264DecRefPicMarking *mark=&s->dec_ref_pic_marking;
    if(idr){if(mark->no_output_of_prior_pics_flag||mark->long_term_reference_flag)return 0;d->count=0;}
    else if(mark->adaptive_ref_pic_marking_mode_flag){
        if(mark->n_ref_pic_marking>10)return 0;
        for(unsigned i=0;i<mark->n_ref_pic_marking;++i){const GstH264RefPicMarking *m=&mark->ref_pic_marking[i];
            if(m->memory_management_control_operation==1){
                if(m->difference_of_pic_nums_minus1>=65536)return 0;
                int32_t target=(int32_t)s->frame_num-(int32_t)m->difference_of_pic_nums_minus1-1;
                unsigned j=0;while(j<d->count&&d->refs[j].pic_num!=target)++j;if(j==d->count)return 0;
                memmove(&d->refs[j],&d->refs[j+1],(--d->count-j)*sizeof(struct ref));
            }else if(m->memory_management_control_operation==5){if(*reset)return 0;d->count=0;*reset=1;}
            else return 0;
        }
    }else if(d->count==limit){
        unsigned old=0;for(unsigned i=1;i<d->count;++i)if(d->refs[i].pic_num<d->refs[old].pic_num)old=i;
        memmove(&d->refs[old],&d->refs[old+1],(--d->count-old)*sizeof(struct ref));
    }
    if(d->count>=limit||d->count>=4)return 0;
    int32_t poc=top<bottom?top:bottom;
    if(*reset){top-=poc;bottom-=poc;poc=0;}
    d->refs[d->count++]=(struct ref){poc,top,bottom,*reset?0:(int32_t)s->frame_num,*reset?0:s->frame_num,slot};return 1;
}
static int geometry(const GstH264SPS *s,struct h264_high_plan *p)
{
    if(s->profile_idc!=100||!s->level_idc||s->level_idc>40||s->chroma_format_idc!=1||s->separate_colour_plane_flag||s->bit_depth_luma_minus8||s->bit_depth_chroma_minus8||s->qpprime_y_zero_transform_bypass_flag||!s->frame_mbs_only_flag||!s->direct_8x8_inference_flag||s->pic_order_cnt_type==1||!s->num_ref_frames||s->num_ref_frames>4||s->gaps_in_frame_num_value_allowed_flag)return 0;
    unsigned w=s->width,h=s->height,vw=s->frame_cropping_flag?s->crop_rect_width:w,vh=s->frame_cropping_flag?s->crop_rect_height:h;
    if(!w||w>1920||!h||h>1088||!vw||vw>1920||!vh||vh>1080||(vw|vh)&1||vw>w||vh>h)return 0;
    const GstH264VUIParams *v=&s->vui_parameters;
    if(!s->vui_parameters_present_flag||!v->bitstream_restriction_flag||v->num_reorder_frames>2||!v->max_dec_frame_buffering||v->max_dec_frame_buffering>4||v->max_dec_frame_buffering<s->num_ref_frames||v->num_reorder_frames>v->max_dec_frame_buffering)return 0;
    unsigned n=s->num_ref_frames+1,y=page(w*h),c=page(w*h/2),field=((w/16)*((h/16+1)/2)*32+1023)&~1023u,mv=page(field*2);
    if(n*y>A733_H264_HIGH_Y_BYTES||n*c>A733_H264_HIGH_C_BYTES||n*mv>A733_H264_HIGH_MV_BYTES)return 0;
    /* Annex A MaxDpbMbs / MaxFS, adapted upstream level table; no rate claim. */
    unsigned fs,dpb;
    switch(s->level_idc){case 10:fs=99;dpb=396;break;case 11:fs=396;dpb=900;break;case 12:case 13:case 20:fs=396;dpb=2376;break;case 21:fs=792;dpb=4752;break;case 22:case 30:fs=1620;dpb=8100;break;case 31:fs=3600;dpb=18000;break;case 32:fs=5120;dpb=20480;break;case 40:fs=8192;dpb=32768;break;default:return 0;}
    unsigned mbs=w/16*(h/16);if(mbs>fs||v->max_dec_frame_buffering>dpb/mbs)return 0;
    p->slots=n;p->y_step=y;p->c_step=c;p->mv_step=mv;p->mv_field=field;p->picture_used=18*4096+h*128;if(p->picture_used<130*1024)p->picture_used=130*1024;
    p->reorder=v->num_reorder_frames;p->dpb_limit=v->max_dec_frame_buffering;
    p->picture.width=w;p->picture.height=h;p->picture.visible_width=vw;p->picture.visible_height=vh;
    p->picture.crop_left=s->frame_cropping_flag?s->crop_rect_x:0;p->picture.crop_top=s->frame_cropping_flag?s->crop_rect_y:0;
    p->picture.crop_right=w-vw-p->picture.crop_left;p->picture.crop_bottom=h-vh-p->picture.crop_top;
    return 1;
}
int h264_high_prepare(struct h264_high *h,const uint8_t *data,size_t bytes,
    struct h264_high_plan *out,size_t *config_bytes)
{
    if(!h||!data||!out||!config_bytes||!bytes||bytes>A733_H264_HIGH_INPUT_BYTES||h->pending_valid)return 0;
    struct h264_high_plan p={0};GstH264NalUnit slice_nal={0};unsigned stage=0;
    GstH264SPS *sps=NULL;GstH264PPS *pps=NULL;size_t config=0;
    for(size_t at=0;at<bytes;){
        size_t sc=prefix(data,bytes,at);if(!sc)return 0;size_t begin=at+sc,end=begin;
        while(end<bytes&&!prefix(data,bytes,end))++end;size_t last=end;while(last>begin&&!data[last-1])--last;
        if(last-begin<2||(data[begin]&128))return 0;
        unsigned zeros=0;
        for(size_t i=begin+1;i<last;++i){unsigned b=data[i];
            if(zeros==2){if(b==3){if(i+1==last||data[i+1]>3)return 0;zeros=0;continue;}if(b<3)return 0;}
            zeros=b==0?zeros+1:0;
        }
        GstH264NalUnit n={.data=(guint8 *)data,.offset=(guint)begin,.size=(guint)(last-begin),.header_bytes=1,.type=data[begin]&31,.ref_idc=(data[begin]>>5)&3,.idr_pic_flag=(data[begin]&31)==5};
        if(n.type==7&&stage==0){
            if(n.size>4096||!n.ref_idc||gst_h264_parse_sps(&n,&h->parser.sps[0])!=GST_H264_PARSER_OK)return 0;
            /* Relocate parsed SPS to its real syntax ID without retaining input. */
            unsigned id=h->parser.sps[0].id;if(id)h->parser.sps[id]=h->parser.sps[0];sps=&h->parser.sps[id];
            if(!geometry(sps,&p))return 0;stage=1;
        }else if(n.type==8&&stage==1){
            if(n.size>4096||!n.ref_idc||gst_h264_parse_pps(&h->parser,&n,&h->parser.pps[0])!=GST_H264_PARSER_OK)return 0;
            unsigned id=h->parser.pps[0].id;if(id)h->parser.pps[id]=h->parser.pps[0];pps=&h->parser.pps[id];
            if(pps->sequence!=sps||pps->num_slice_groups_minus1||pps->redundant_pic_cnt_present_flag||pps->weighted_bipred_idc>2)return 0;
            config=end;stage=2;
        }else if((n.type==1||n.type==5)&&stage==2){slice_nal=n;stage=3;}
        else return 0;
        at=end;
    }
    if(stage!=3||!sps||!pps||gst_h264_parser_parse_slice_hdr(&h->parser,&slice_nal,&h->slice,TRUE,TRUE)!=GST_H264_PARSER_OK)return 0;
    const GstH264SliceHdr *s=&h->slice;unsigned type=s->type%5,ref=slice_nal.ref_idc,idr=slice_nal.idr_pic_flag;
    if(type>2||s->pps!=pps||s->first_mb_in_slice||s->field_pic_flag||s->redundant_pic_cnt||s->cabac_init_idc>2||s->disable_deblocking_filter_idc>2||(!h->committed.started&&!idr)||(idr&&(type!=2||s->frame_num||!ref)))return 0;
    int q=26+pps->pic_init_qp_minus26+s->slice_qp_delta;if(q<0||q>51)return 0;
    struct dpb d=h->committed;
    if(idr)d=(struct dpb){.gop=h->committed.started?h->committed.gop+1:0};
    unsigned maxframe=1u<<(sps->log2_max_frame_num_minus4+4);
    /* Upstream gap check uses the previous reference, not the previous B. */
    if(!idr&&s->frame_num!=d.prev_ref_frame&&s->frame_num!=(d.prev_ref_frame+1)%maxframe)return 0;
    for(unsigned i=0;i<d.count;++i)d.refs[i].pic_num=d.refs[i].frame>s->frame_num?(int32_t)d.refs[i].frame-(int32_t)maxframe:(int32_t)d.refs[i].frame;
    uint32_t offset=idr?0:d.frame_offset+(s->frame_num<d.prev_frame?maxframe:0);
    if(offset>16777216)return 0;
    int32_t msb=0,top,bottom;
    if(sps->pic_order_cnt_type==0){
        int32_t maxpoc=1<<(sps->log2_max_pic_order_cnt_lsb_minus4+4),lsb=s->pic_order_cnt_lsb;
        msb=d.prev_msb;
        if(lsb<d.prev_lsb&&d.prev_lsb-lsb>=maxpoc/2)msb+=maxpoc;
        else if(lsb>d.prev_lsb&&lsb-d.prev_lsb>maxpoc/2)msb-=maxpoc;
        if(msb < -16777216||msb>16777216||s->delta_pic_order_cnt_bottom < -65535||s->delta_pic_order_cnt_bottom>65535)return 0;
        top=msb+lsb;bottom=top+s->delta_pic_order_cnt_bottom;
    }else top=bottom=idr?0:2*(int32_t)(offset+s->frame_num)-(ref?0:1);
    int32_t poc=top<bottom?top:bottom;
    unsigned slot=0;for(;slot<p.slots;++slot){unsigned used=0;for(unsigned i=0;i<d.count;++i)used|=d.refs[i].slot==slot;if(!used)break;}
    if(slot==p.slots)return 0;
    struct ref list[2][8]={0};for(unsigned l=0;l<2;++l)for(unsigned i=0;i<8;++i){list[l][i].slot=UINT32_MAX;list[l][i].pic_num=INT32_MIN;}
    unsigned available[2];defaults(&d,type,poc,list,available);
    unsigned active[2]={type==2?0:(unsigned)s->num_ref_idx_l0_active_minus1+1,type==1?(unsigned)s->num_ref_idx_l1_active_minus1+1:0};
    for(unsigned l=0;l<2;++l){
        if(active[l]>4||!modify(&d,s,l,maxframe,list[l],active[l]))return 0;
        p.ref_count[l]=active[l];for(unsigned i=0;i<active[l];++i){if(list[l][i].slot>=p.slots)return 0;p.lists[l][i]=list[l][i].slot;}
    }
    for(unsigned i=0;i<d.count;++i){unsigned k=d.refs[i].slot;p.valid[k]=1;p.pocs[k][0]=(uint32_t)d.refs[i].top;p.pocs[k][1]=(uint32_t)d.refs[i].bottom;}
    p.slot=slot;p.valid[slot]=1;p.pocs[slot][0]=(uint32_t)top;p.pocs[slot][1]=(uint32_t)bottom;
    unsigned reset;
    if(!marking(&d,s,idr,ref,slot,top,bottom,sps->num_ref_frames,&reset))return 0;
    d.started=1;d.prev_frame=reset?0:s->frame_num;d.frame_offset=reset?0:offset;
    if(ref){d.prev_ref_frame=reset?0:s->frame_num;d.prev_msb=reset?0:msb;d.prev_lsb=reset?top-poc:s->pic_order_cnt_lsb;}
    if(reset)++d.gop;
    p.gop=d.gop;p.reference=ref;p.idr=idr;p.mmco5=reset;p.poc_bottom=(uint32_t)bottom;
    p.picture.frame_num=s->frame_num;p.picture.poc=reset?0:(uint32_t)poc;p.picture.slice_type=type;
    p.picture.slice_offset=slice_nal.offset;p.picture.slice_bytes=slice_nal.size;
    p.picture.header_bits=8+s->header_size-8*s->n_emulation_prevention_bytes;
    if(p.picture.header_bits>=8*slice_nal.size)return 0;
    p.picture.sps=0xd0000|((sps->pic_width_in_mbs_minus1&255)<<8)|sps->pic_height_in_map_units_minus1;
    p.picture.pps=(pps->num_ref_idx_l0_active_minus1<<10)|(pps->num_ref_idx_l1_active_minus1<<5)|(pps->weighted_bipred_idc<<2)|(pps->entropy_coding_mode_flag<<15)|(pps->weighted_pred_flag<<4)|(pps->constrained_intra_pred_flag<<1)|pps->transform_8x8_mode_flag;
    p.picture.shs=0x20|(ref?0x1000:0)|(type<<8)|(s->direct_spatial_mv_pred_flag?0x4:0)|s->cabac_init_idc;
    p.picture.shs2=0x1000|((active[0]?active[0]-1:0)<<24)|((active[1]?active[1]-1:0)<<16)|(s->disable_deblocking_filter_idc<<8)|(((uint32_t)s->slice_alpha_c0_offset_div2&15)<<4)|((uint32_t)s->slice_beta_offset_div2&15);
    p.custom_scaling=sps->scaling_matrix_present_flag||pps->pic_scaling_matrix_present_flag;
    p.picture.qp=(p.custom_scaling?0:0x1000000)|(((uint32_t)pps->second_chroma_qp_index_offset&63)<<16)|(((uint32_t)pps->chroma_qp_index_offset&63)<<8)|(unsigned)q;
    /* Cedrus scaling order: two raster8x8 lists, then six raster4x4. */
    uint8_t scale[224];gst_h264_quant_matrix_8x8_get_raster_from_zigzag(scale,pps->scaling_lists_8x8[0]);gst_h264_quant_matrix_8x8_get_raster_from_zigzag(scale+64,pps->scaling_lists_8x8[3]);
    for(unsigned i=0;i<6;++i)gst_h264_quant_matrix_4x4_get_raster_from_zigzag(scale+128+16*i,pps->scaling_lists_4x4[i]);
    for(unsigned i=0;i<56;++i)p.scaling[i]=(uint32_t)scale[i*4]|((uint32_t)scale[i*4+1]<<8)|((uint32_t)scale[i*4+2]<<16)|((uint32_t)scale[i*4+3]<<24);
    const GstH264PredWeightTable *w=&s->pred_weight_table;unsigned ld=w->luma_log2_weight_denom,cd=w->chroma_log2_weight_denom;
    if(type==1&&pps->weighted_bipred_idc==2)ld=cd=5;
    if(ld>7||cd>7)return 0;p.weight_denom=ld|(cd<<4);
    for(unsigned l=0;l<2;++l)for(unsigned i=0;i<32;++i){
        int weight=l?w->luma_weight_l1[i]:w->luma_weight_l0[i],off=l?w->luma_offset_l1[i]:w->luma_offset_l0[i];
        if((type==1&&pps->weighted_bipred_idc==2)||i>=active[l]){weight=1<<ld;off=0;}
        p.weights[l*96+i]=((uint32_t)weight&511)|(((uint32_t)off&511)<<16);
        for(unsigned c=0;c<2;++c){weight=l?w->chroma_weight_l1[i][c]:w->chroma_weight_l0[i][c];off=l?w->chroma_offset_l1[i][c]:w->chroma_offset_l0[i][c];
            if((type==1&&pps->weighted_bipred_idc==2)||i>=active[l]){weight=1<<cd;off=0;}
            p.weights[l*96+32+i*2+c]=((uint32_t)weight&511)|(((uint32_t)off&511)<<16);
        }
    }
    h->pending=d;h->prepared=p;h->pending_valid=1;*out=p;*config_bytes=config;return 1;
}
int h264_high_commit(struct h264_high *h,const struct h264_high_plan *p)
{if(!h||!p||!h->pending_valid||memcmp(p,&h->prepared,sizeof(*p)))return 0;h->committed=h->pending;h->pending_valid=0;return 1;}
