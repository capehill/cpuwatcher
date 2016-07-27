/*

This code piece is heavily influenced (if not blatantly ripped from)
by Olaf Barthel's "sample.c" example code.

I can take the credit for possible bugs added ;)

*/

#include <proto/bsdsocket.h>
#include <stdio.h>

static ULONG quad_delta(const SBQUAD_T * a, const SBQUAD_T * b)
{
	ULONG result = 0;
	SBQUAD_T delta;

	delta = (*a);

	if (delta.sbq_Low < b->sbq_Low)
	{
		if (delta.sbq_High == 0)
			goto out;

		delta.sbq_High--;

		delta.sbq_Low = (~0UL) - b->sbq_Low + 1 + a->sbq_Low;
	}
	else
	{
		delta.sbq_Low -= b->sbq_Low;
	}

	if (delta.sbq_High < b->sbq_High)
		goto out;

	/*delta.sbq_High -= b->sbq_High;*/

	result = delta.sbq_Low;

out:

	return(result);
}


static SBQUAD_T	LastSampleIn;
static SBQUAD_T	LastSampleOut;


void init_netstats(void)
{
 	if ( SocketBaseTags(
 		SBTM_GETREF(SBTC_GET_BYTES_RECEIVED), &LastSampleIn,
 		SBTM_GETREF(SBTC_GET_BYTES_SENT), &LastSampleOut,
        TAG_END) )
 	{
 		printf("Could not query data throughput statistics.\n");
 	}
}


/*

Check out what has been going on through the network. Update parameter
values, and return TRUE if the graph data should be rescaled with the multiplier.

Recalculation should happen when a new peak in the transfers has happened.

*/
BOOL update_netstats(UBYTE *download, UBYTE *upload, float *dl_multiplier, float *ul_multiplier,
	float *dl_speed, float *ul_speed)
{
	static ULONG MaxSent;
    //static ULONG MaxSentReceived;
    static ULONG MaxReceived;

	SBQUAD_T sample_in;
	SBQUAD_T sample_out;

    ULONG received, sent;

	BOOL redraw = FALSE;

 	if ( SocketBaseTags(
 		SBTM_GETREF(SBTC_GET_BYTES_RECEIVED), &sample_in,
 		SBTM_GETREF(SBTC_GET_BYTES_SENT), &sample_out,
        TAG_END) )
 	{
 		printf("Could not query data throughput statistics.\n");
 		*upload = *download = *ul_speed = *dl_speed = 0;
        return FALSE;
 	}

	received = quad_delta(&sample_in, &LastSampleIn);
	sent = quad_delta(&sample_out, &LastSampleOut);

	LastSampleIn = sample_in;
	LastSampleOut = sample_out;

	if (sent > MaxSent)
	{
        //printf("MaxSent changes to %ld\n", sent);

        *ul_multiplier = (float) MaxSent / sent;
        redraw = TRUE;
		MaxSent = sent;
/*
		if (sent > MaxSentReceived)
        {
			MaxSentReceived = sent;
        }
    */
	}

	if (received > MaxReceived)
	{
        //printf("MaxReceived changes to %ld\n", received);

        *dl_multiplier = (float) MaxReceived / received;
		redraw = TRUE;
        MaxReceived = received;
/*
		if (received > MaxSentReceived)
		{
			MaxSentReceived = received;
        }
*/
	}

    // Scale the values to 0 ... 100
    if ( /*MaxSentReceived*/ MaxSent > 0 )
    {
    	*upload = 100 * (float) sent / MaxSent;//Received;
    }
    else
    {
        *upload = 0;
    }

    if ( MaxReceived > 0 )
    {
    	*download = 100 * (float) received / MaxReceived; //SentReceived;
    }
    else
    {
        *download = 0;
    }

    // Calculate current up/down load speeds in Kilobytes
    *ul_speed = (float) sent / 1024;
    *dl_speed = (float) received / 1024;

    //printf("s %d r %d m %f\n", *download, *upload, *multiplier);

    return redraw;
}
