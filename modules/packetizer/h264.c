/*****************************************************************************
 * h264.c: h264/avc video packetizer
 *****************************************************************************
 * Copyright (C) 2001, 2002 VideoLAN
 * $Id$
 *
 * Authors: Laurent Aimar <fenrir@via.ecp.fr>
 *          Eric Petit <titer@videolan.org>
 *          Gildas Bazin <gbazin@netcourrier.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/
#include <stdlib.h>                                      /* malloc(), free() */

#include <vlc/vlc.h>
#include <vlc/decoder.h>
#include <vlc/sout.h>

#include "vlc_block_helper.h"
#include "vlc_bits.h"

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
static int  Open ( vlc_object_t * );
static void Close( vlc_object_t * );

vlc_module_begin();
    set_description( _("H264 video packetizer") );
    set_capability( "packetizer", 50 );
    set_callbacks( Open, Close );
vlc_module_end();


/****************************************************************************
 * Local prototypes
 ****************************************************************************/
static block_t *Packetize( decoder_t *, block_t ** );

struct decoder_sys_t
{
    block_bytestream_t bytestream;

    int     i_state;
    int     i_offset;
    uint8_t startcode[4];

    vlc_bool_t b_slice;
    block_t    *p_frame;

    int64_t      i_dts;
    int64_t      i_pts;
    unsigned int i_flags;

    vlc_bool_t   b_sps;
};

enum
{
    STATE_NOSYNC,
    STATE_NEXT_SYNC,
};

enum nal_unit_type_e
{
    NAL_UNKNOWN = 0,
    NAL_SLICE   = 1,
    NAL_SLICE_DPA   = 2,
    NAL_SLICE_DPB   = 3,
    NAL_SLICE_DPC   = 4,
    NAL_SLICE_IDR   = 5,    /* ref_idc != 0 */
    NAL_SEI         = 6,    /* ref_idc == 0 */
    NAL_SPS         = 7,
    NAL_PPS         = 8
    /* ref_idc == 0 for 6,9,10,11,12 */
};

enum nal_priority_e
{
    NAL_PRIORITY_DISPOSABLE = 0,
    NAL_PRIORITY_LOW        = 1,
    NAL_PRIORITY_HIGH       = 2,
    NAL_PRIORITY_HIGHEST    = 3,
};

static block_t *ParseNALBlock( decoder_t *, block_t * );

/*****************************************************************************
 * Open: probe the packetizer and return score
 *****************************************************************************/
static int Open( vlc_object_t *p_this )
{
    decoder_t     *p_dec = (decoder_t*)p_this;
    decoder_sys_t *p_sys;

    if( p_dec->fmt_in.i_codec != VLC_FOURCC( 'h', '2', '6', '4') &&
        p_dec->fmt_in.i_codec != VLC_FOURCC( 'H', '2', '6', '4') )
    {
        return VLC_EGENERIC;
    }

    /* Allocate the memory needed to store the decoder's structure */
    if( ( p_dec->p_sys = p_sys = malloc( sizeof(decoder_sys_t) ) ) == NULL )
    {
        msg_Err( p_dec, "out of memory" );
        return VLC_EGENERIC;
    }
    p_sys->i_state = STATE_NOSYNC;
    p_sys->i_offset = 0;
    p_sys->startcode[0] = 0;
    p_sys->startcode[1] = 0;
    p_sys->startcode[2] = 0;
    p_sys->startcode[3] = 1;
    p_sys->bytestream = block_BytestreamInit( p_dec );
    p_sys->b_slice = VLC_FALSE;
    p_sys->p_frame = NULL;
    p_sys->i_dts   = 0;
    p_sys->i_pts   = 0;
    p_sys->i_flags = 0;
    p_sys->b_sps   = VLC_FALSE;

    /* Setup properties */
    es_format_Copy( &p_dec->fmt_out, &p_dec->fmt_in );
    p_dec->fmt_out.i_codec = VLC_FOURCC( 'h', '2', '6', '4' );

#if 0
    if( p_dec->fmt_in.i_extra )
    {
        /* We have a vol */
        p_dec->fmt_out.i_extra = p_dec->fmt_in.i_extra;
        p_dec->fmt_out.p_extra = malloc( p_dec->fmt_in.i_extra );
        memcpy( p_dec->fmt_out.p_extra, p_dec->fmt_in.p_extra,
                p_dec->fmt_in.i_extra );

        msg_Dbg( p_dec, "opening with vol size:%d", p_dec->fmt_in.i_extra );
        m4v_VOLParse( &p_dec->fmt_out,
                      p_dec->fmt_out.p_extra, p_dec->fmt_out.i_extra );
    }
    else
    {
        /* No vol, we'll have to look for one later on */
        p_dec->fmt_out.i_extra = 0;
        p_dec->fmt_out.p_extra = 0;
    }
#endif

    /* Set callback */
    p_dec->pf_packetize = Packetize;

    return VLC_SUCCESS;
}

/*****************************************************************************
 * Close: clean up the packetizer
 *****************************************************************************/
static void Close( vlc_object_t *p_this )
{
    decoder_t *p_dec = (decoder_t*)p_this;
    decoder_sys_t *p_sys = p_dec->p_sys;

    block_BytestreamRelease( &p_sys->bytestream );
    free( p_sys );
}

/****************************************************************************
 * Packetize: the whole thing
 ****************************************************************************/
static block_t *Packetize( decoder_t *p_dec, block_t **pp_block )
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    block_t       *p_pic;

    if( !pp_block || !*pp_block ) return NULL;

    block_BytestreamPush( &p_sys->bytestream, *pp_block );

    for( ;; )
    {
        switch( p_sys->i_state )
        {
            case STATE_NOSYNC:
                if( block_FindStartcodeFromOffset( &p_sys->bytestream,
                        &p_sys->i_offset, p_sys->startcode, 4 ) == VLC_SUCCESS )
                {
                    p_sys->i_state = STATE_NEXT_SYNC;
                }

                if( p_sys->i_offset )
                {
                    block_SkipBytes( &p_sys->bytestream, p_sys->i_offset );
                    p_sys->i_offset = 0;
                    block_BytestreamFlush( &p_sys->bytestream );
                }

                if( p_sys->i_state != STATE_NEXT_SYNC )
                {
                    /* Need more data */
                    return NULL;
                }

                p_sys->i_offset = 1; /* To find next startcode */

            case STATE_NEXT_SYNC:
                /* Find the next startcode */
                if( block_FindStartcodeFromOffset( &p_sys->bytestream,
                        &p_sys->i_offset, p_sys->startcode, 4 ) != VLC_SUCCESS )
                {
                    /* Need more data */
                    return NULL;
                }

                /* Get the new fragment and set the pts/dts */
                p_pic = block_New( p_dec, p_sys->i_offset );
                p_pic->i_pts = p_sys->bytestream.p_block->i_pts;
                p_pic->i_dts = p_sys->bytestream.p_block->i_dts;

                block_GetBytes( &p_sys->bytestream, p_pic->p_buffer,
                                p_pic->i_buffer );

                p_sys->i_offset = 0;

                /* Parse the NAL */
                if( !( p_pic = ParseNALBlock( p_dec, p_pic ) ) )
                {
                    p_sys->i_state = STATE_NOSYNC;
                    break;
                }

                /* So p_block doesn't get re-added several times */
                *pp_block = block_BytestreamPop( &p_sys->bytestream );

                p_sys->i_state = STATE_NOSYNC;

                return p_pic;
        }
    }
}

static void nal_get_decoded( uint8_t **pp_ret, int *pi_ret, uint8_t *src, int i_src )
{
    uint8_t *end = &src[i_src];
    uint8_t *dst = malloc( i_src );

    *pp_ret = dst;

    while( src < end )
    {
        if( src < end - 3 && src[0] == 0x00 && src[1] == 0x00  && src[2] == 0x03 )
        {
            *dst++ = 0x00;
            *dst++ = 0x00;

            src += 3;
            continue;
        }
        *dst++ = *src++;
    }

    *pi_ret = dst - *pp_ret;
}

static inline int bs_read_ue( bs_t *s )
{
    int i = 0;

    while( bs_read1( s ) == 0 && s->p < s->p_end && i < 32 )
    {
        i++;
    }
    return( ( 1 << i) - 1 + bs_read( s, i ) );
}
static inline int bs_read_se( bs_t *s ) 
{
    int val = bs_read_ue( s );

    return val&0x01 ? (val+1)/2 : -(val/2);
}


static block_t *ParseNALBlock( decoder_t *p_dec, block_t *p_frag )
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    block_t *p_pic = NULL;

    const int i_ref_idc = (p_frag->p_buffer[4] >> 5)&0x03;
    const int i_nal_type= p_frag->p_buffer[4]&0x1f;

    if( p_sys->b_slice &&
        ( i_nal_type == NAL_SLICE || i_nal_type == NAL_SLICE_IDR ||
          i_nal_type == NAL_SLICE_DPC || i_nal_type == NAL_SPS || i_nal_type == NAL_PPS ) )
    {
        if( p_sys->b_sps )
        {
            p_pic = block_ChainGather( p_sys->p_frame );
            p_pic->i_dts = p_sys->i_dts;
            p_pic->i_pts = p_sys->i_pts;
            p_pic->i_length = 0;    /* FIXME */
            p_pic->i_flags = p_sys->i_flags;
        }
        else
        {
            block_ChainRelease( p_sys->p_frame );
        }

        /* reset context */
        p_sys->p_frame = NULL;
        p_sys->b_slice = VLC_FALSE;
        //p_sys->i_dts += 40000;
    }

    if( i_nal_type >= NAL_SLICE && i_nal_type <= NAL_SLICE_IDR )
    {
        uint8_t *dec;
        int     i_dec;
        bs_t s;

        p_sys->b_slice = VLC_TRUE;
        p_sys->i_dts   = p_frag->i_dts;
        p_sys->i_pts   = p_frag->i_pts;

        /* do not convert the whole frame */
        nal_get_decoded( &dec, &i_dec, &p_frag->p_buffer[5], __MIN( p_frag->i_buffer - 5, 60 ) );
        bs_init( &s, dec, i_dec );

        /* i_first_mb */
        bs_read_ue( &s );
        /* picture type */
        switch( bs_read_ue( &s ) )
        {
            case 0: case 5:
                p_sys->i_flags = BLOCK_FLAG_TYPE_P;
                break;
            case 1: case 6:
                p_sys->i_flags =BLOCK_FLAG_TYPE_B;
                break;
            case 2: case 7:
                p_sys->i_flags = BLOCK_FLAG_TYPE_I;
                break;
            case 3: case 8: /* SP */
                p_sys->i_flags = BLOCK_FLAG_TYPE_P;
                break;
            case 4: case 9:
                p_sys->i_flags = BLOCK_FLAG_TYPE_I;
                break;
        }

        free( dec );
    }
    else if( i_nal_type == NAL_SPS )
    {
        uint8_t *dec;
        int     i_dec;
        bs_t s;
        int i_tmp;

        p_sys->b_sps = VLC_TRUE;

        nal_get_decoded( &dec, &i_dec, &p_frag->p_buffer[5], p_frag->i_buffer - 5 );

        bs_init( &s, dec, i_dec );
        /* Skip profile(8), constraint_set012, reserver(5), level(8) */
        bs_skip( &s, 8 + 1+1+1 + 5 + 8 );
        /* sps id */
        bs_read_ue( &s );
        /* Skip i_log2_max_frame_num */
        bs_read_ue( &s );
        /* Read poc_type */
        i_tmp = bs_read_ue( &s );
        if( i_tmp == 0 )
        {
            /* skip i_log2_max_poc_lsb */
            bs_read_ue( &s );
        }
        else if( i_tmp == 1 )
        {
            int i_cycle;
            /* skip b_delta_pic_order_always_zero */
            bs_skip( &s, 1 );
            /* skip i_offset_for_non_ref_pic */
            bs_read_se( &s );
            /* skip i_offset_for_top_to_bottom_field */
            bs_read_se( &s );
            /* read i_num_ref_frames_in_poc_cycle */
            i_cycle = bs_read_ue( &s );
            if( i_cycle > 256 ) i_cycle = 256;
            while( i_cycle > 0 )
            {
                /* skip i_offset_for_ref_frame */
                bs_read_se(&s );
            }
        }
        /* i_num_ref_frames */
        bs_read_ue( &s );
        /* b_gaps_in_frame_num_value_allowed */
        bs_skip( &s, 1 );

        /* Read size */
        p_dec->fmt_out.video.i_width  = 16 * ( bs_read_ue( &s ) + 1 );
        p_dec->fmt_out.video.i_height = 16 * ( bs_read_ue( &s ) + 1 );

        /* b_frame_mbs_only */
        i_tmp = bs_read( &s, 1 );
        if( i_tmp == 0 )
        {
            bs_skip( &s, 1 );
        }
        /* b_direct8x8_inference */
        bs_skip( &s, 1 );

        /* crop ? */
        i_tmp = bs_read( &s, 1 );
        if( i_tmp )
        {
            /* left */
            p_dec->fmt_out.video.i_width -= 2 * bs_read_ue( &s );
            /* right */
            p_dec->fmt_out.video.i_width -= 2 * bs_read_ue( &s );
            /* top */
            p_dec->fmt_out.video.i_height -= 2 * bs_read_ue( &s );
            /* bottom */
            p_dec->fmt_out.video.i_height -= 2 * bs_read_ue( &s );
        }

        /* vui */
        i_tmp = bs_read( &s, 1 );
        if( i_tmp )
        {
            /* read the aspect ratio part if any FIXME check it */
            i_tmp = bs_read( &s, 1 );
            if( i_tmp )
            {
                static const struct { int w, h; } sar[14] =
                {
                    { 0,   0 }, { 1,   1 }, { 12, 11 }, { 10, 11 },
                    { 16, 11 }, { 40, 33 }, { 24, 11 }, { 20, 11 },
                    { 32, 11 }, { 80, 33 }, { 18, 11 }, { 15, 11 },
                    { 64, 33 }, { 160,99 },
                };
                int i_sar = bs_read( &s, 8 );
                int w, h;

                if( i_sar < 14 )
                {
                    w = sar[i_sar].w;
                    h = sar[i_sar].h;
                }
                else
                {
                    w = bs_read( &s, 16 );
                    h = bs_read( &s, 16 );
                }
                p_dec->fmt_out.video.i_aspect =
                    VOUT_ASPECT_FACTOR *
                    w / h *
                    p_dec->fmt_out.video.i_width / p_dec->fmt_out.video.i_height;
            }
        }

        free( dec );
    }
    else if( i_nal_type == NAL_PPS )
    {
        bs_t s;
        bs_init( &s, &p_frag->p_buffer[5], p_frag->i_buffer - 5 );

        /* TODO */
    }


    /* Append the block */
    block_ChainAppend( &p_sys->p_frame, p_frag );

    return p_pic;
}

