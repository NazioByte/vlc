/*****************************************************************************
 * lpcm_decoder_thread.c: lpcm decoder thread
 *****************************************************************************
 * Copyright (C) 1999-2001 VideoLAN
 * $Id: lpcm_adec.c,v 1.4 2001/12/09 17:01:36 sam Exp $
 *
 * Authors: Samuel Hocevar <sam@zoy.org>
 *          Henri Fallon <henri@videolan.org>
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

#define MODULE_NAME lpcm_adec
#include "modules_inner.h"

/*****************************************************************************
 * Preamble
 *****************************************************************************/
#include "defs.h"

#ifdef HAVE_UNISTD_H
#include <unistd.h>                                              /* getpid() */
#endif

#include <stdio.h>                                           /* "intf_msg.h" */
#include <string.h>                                    /* memcpy(), memset() */
#include <stdlib.h>                                      /* malloc(), free() */

#include "common.h"
#include "intf_msg.h"                        /* intf_DbgMsg(), intf_ErrMsg() */
#include "threads.h"
#include "mtime.h"

#include "audio_output.h"

#include "modules.h"
#include "modules_export.h"

#include "stream_control.h"
#include "input_ext-dec.h"

#include "lpcm_adec.h"

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
static int      lpcm_adec_Probe         ( probedata_t * );
static int      lpcm_adec_Run           ( decoder_config_t * );
void            lpcm_adec_DecodeFrame   ( lpcmdec_thread_t * );
static int      lpcm_adec_Init          ( lpcmdec_thread_t * );
static void     lpcm_adec_ErrorThread   ( lpcmdec_thread_t * );
static void     lpcm_adec_EndThread     ( lpcmdec_thread_t * );


/*****************************************************************************
 * Capabilities
 *****************************************************************************/
void _M( adec_getfunctions )( function_list_t * p_function_list )
{
    p_function_list->pf_probe = lpcm_adec_Probe;
    p_function_list->functions.dec.pf_run = lpcm_adec_Run;
}

/*****************************************************************************
 * Build configuration tree.
 *****************************************************************************/
MODULE_CONFIG_START
ADD_WINDOW( "Configuration for lpcm audio decoder module" )
    ADD_COMMENT( "Nothing to configure" )
MODULE_CONFIG_STOP

MODULE_INIT_START
    p_module->i_capabilities = MODULE_CAPABILITY_DEC;
    p_module->psz_longname = "Linear PCM audio decoder";
MODULE_INIT_STOP

MODULE_ACTIVATE_START
    _M( adec_getfunctions )( &p_module->p_functions->dec );
MODULE_ACTIVATE_STOP

MODULE_DEACTIVATE_START
MODULE_DEACTIVATE_STOP

/*****************************************************************************
 * lpcm_adec_Probe: probe the decoder and return score
 *****************************************************************************/
static int lpcm_adec_Probe( probedata_t *p_data )
{
    if( p_data->i_type == LPCM_AUDIO_ES )
        return( 100 );
    else
        return( 0 );
}

/*****************************************************************************
 * lpcm_adec_Run: the lpcm decoder
 *****************************************************************************/
static int lpcm_adec_Run( decoder_config_t * p_config )
{
    lpcmdec_thread_t *   p_lpcmdec;

    intf_DbgMsg("lpcm_adec debug: thread launched, initializing.");
    
    /* Allocate the memory needed to store the thread's structure */
    if( (p_lpcmdec = (lpcmdec_thread_t *)malloc (sizeof(lpcmdec_thread_t)) )
            == NULL) 
    {
        intf_ErrMsg( "LPCM : error : cannot create lpcmdec_thread_t" );
        return( -1 );
    }

    /*
     * Initialize the thread properties
     */
    p_lpcmdec->p_config = p_config;
    p_lpcmdec->p_fifo = p_config->p_decoder_fifo;

    if( lpcm_adec_Init( p_lpcmdec ) )
    {
        return( -1 );
    }

    intf_DbgMsg( "LPCM Debug: lpcm decoder thread %p initialized\n", 
                 p_lpcmdec );
    
    /* lpcm decoder thread's main loop */
    while ((!p_lpcmdec->p_fifo->b_die) && (!p_lpcmdec->p_fifo->b_error))
    {
        lpcm_adec_DecodeFrame(p_lpcmdec);
    }

    /* If b_error is set, the lpcm decoder thread enters the error loop */
    if (p_lpcmdec->p_fifo->b_error)
    {
        lpcm_adec_ErrorThread (p_lpcmdec);
    }

    /* End of the lpcm decoder thread */
    lpcm_adec_EndThread (p_lpcmdec);

    return( 0 );
}

/*****************************************************************************
 * lpcm_adec_Init : initialize an lpcm decoder thread
 *****************************************************************************/
static int lpcm_adec_Init (lpcmdec_thread_t * p_lpcmdec)
{

    /* Init the BitStream */
    p_lpcmdec->p_config->pf_init_bit_stream(
            &p_lpcmdec->bit_stream,
            p_lpcmdec->p_config->p_decoder_fifo,
            NULL, NULL);

    /* Creating the audio output fifo */
    p_lpcmdec->p_aout_fifo = aout_CreateFifo( AOUT_ADEC_STEREO_FIFO, 2, 48000,
                                            0, LPCMDEC_FRAME_SIZE/2, NULL  );
    if ( p_lpcmdec->p_aout_fifo == NULL )
    {
        return( -1 );
    }
    return( 0 );
}

/*****************************************************************************
 * lpcm_adec_DecodeFrame: decodes a frame.
 *****************************************************************************/
void lpcm_adec_DecodeFrame( lpcmdec_thread_t * p_lpcmdec )
{
    byte_t * buffer,p_temp[LPCMDEC_FRAME_SIZE];
    int i_loop;
    byte_t byte1, byte2;

    if( DECODER_FIFO_START(*p_lpcmdec->p_fifo)->i_pts )
    {
        p_lpcmdec->p_aout_fifo->date[p_lpcmdec->p_aout_fifo->l_end_frame] =
            DECODER_FIFO_START(*p_lpcmdec->p_fifo)->i_pts;
        DECODER_FIFO_START(*p_lpcmdec->p_fifo)->i_pts = 0;
    }
    else
    { 
        p_lpcmdec->p_aout_fifo->date[p_lpcmdec->p_aout_fifo->l_end_frame] =
            LAST_MDATE;
    }

    buffer = ((byte_t *)p_lpcmdec->p_aout_fifo->buffer) + 
              (p_lpcmdec->p_aout_fifo->l_end_frame * LPCMDEC_FRAME_SIZE);

    RemoveBits32(&p_lpcmdec->bit_stream);
    byte1 = GetBits(&p_lpcmdec->bit_stream, 8) ;
    byte2 = GetBits(&p_lpcmdec->bit_stream, 8) ;
    
    /* I only have 2 test streams. As far as I understand
     * after the RemoveBits and the 2 GetBits, we should be exactly 
     * where we whant : the sync word : 0x0180.
     * If not, we got and find it. */
    while( ( byte1 != 0x01 || byte2 != 0x80 ) && (!p_lpcmdec->p_fifo->b_die)
                                       && (!p_lpcmdec->p_fifo->b_error) )
    {
        byte1 = byte2;
        byte2 = GetBits(&p_lpcmdec->bit_stream, 8);
    }
    
    GetChunk( &p_lpcmdec->bit_stream, p_temp, LPCMDEC_FRAME_SIZE);
    
    for( i_loop = 0; i_loop < LPCMDEC_FRAME_SIZE/2; i_loop++ )
    {
        buffer[2*i_loop]=p_temp[2*i_loop+1];
        buffer[2*i_loop+1]=p_temp[2*i_loop];
    }
    
    vlc_mutex_lock (&p_lpcmdec->p_aout_fifo->data_lock);
    p_lpcmdec->p_aout_fifo->l_end_frame = 
        (p_lpcmdec->p_aout_fifo->l_end_frame + 1) & AOUT_FIFO_SIZE;
    vlc_cond_signal (&p_lpcmdec->p_aout_fifo->data_wait);
    vlc_mutex_unlock (&p_lpcmdec->p_aout_fifo->data_lock);
    
    intf_DbgMsg( "LPCM Debug: %x", *buffer );

}


/*****************************************************************************
 * lpcm_adec_ErrorThread : lpcm decoder's RunThread() error loop
 *****************************************************************************/
static void lpcm_adec_ErrorThread( lpcmdec_thread_t * p_lpcmdec )
{
    /* We take the lock, because we are going to read/write the start/end
     * indexes of the decoder fifo */
    vlc_mutex_lock( &p_lpcmdec->p_fifo->data_lock );

    /* Wait until a `die' order is sent */
    while( !p_lpcmdec->p_fifo->b_die ) 
    {
        /* Trash all received PES packets */
        while( !DECODER_FIFO_ISEMPTY(*p_lpcmdec->p_fifo) ) 
        {
            p_lpcmdec->p_fifo->pf_delete_pes( p_lpcmdec->p_fifo->p_packets_mgt,
                    DECODER_FIFO_START(*p_lpcmdec->p_fifo ));
            DECODER_FIFO_INCSTART( *p_lpcmdec->p_fifo );
        }

        /* Waiting for the input thread to put new PES packets in the fifo */
        vlc_cond_wait ( &p_lpcmdec->p_fifo->data_wait, 
                        &p_lpcmdec->p_fifo->data_lock );
    }

    /* We can release the lock before leaving */
    vlc_mutex_unlock( &p_lpcmdec->p_fifo->data_lock );
}

/*****************************************************************************
 * lpcm_adec_EndThread : lpcm decoder thread destruction
 *****************************************************************************/
static void lpcm_adec_EndThread( lpcmdec_thread_t * p_lpcmdec )
{
    intf_DbgMsg( "LPCM Debug: destroying lpcm decoder thread %p", p_lpcmdec );

    /* If the audio output fifo was created, we destroy it */
    if( p_lpcmdec->p_aout_fifo != NULL ) 
    {
        aout_DestroyFifo( p_lpcmdec->p_aout_fifo );

        /* Make sure the output thread leaves the NextFrame() function */
        vlc_mutex_lock( &(p_lpcmdec->p_aout_fifo->data_lock) );
        vlc_cond_signal( &(p_lpcmdec->p_aout_fifo->data_wait) );
        vlc_mutex_unlock( &(p_lpcmdec->p_aout_fifo->data_lock) );
    }

    /* Destroy descriptor */
    free( p_lpcmdec );

    intf_DbgMsg( "LPCM Debug: lpcm decoder thread %p destroyed", p_lpcmdec );
}
