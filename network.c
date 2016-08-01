/*

This code piece is heavily influenced by Olaf Barthel's "sample.c" example code.

*/

#include <proto/bsdsocket.h>
#include <stdio.h>

static ULONG quad_delta(const SBQUAD_T *a, const SBQUAD_T *b)
{
	ULONG result = 0;
	SBQUAD_T delta;

	delta = (*a);

	if (delta.sbq_Low < b->sbq_Low) {
		if (delta.sbq_High == 0)
			goto out;

		delta.sbq_High--;

		delta.sbq_Low = (~0UL) - b->sbq_Low + 1 + a->sbq_Low;
	} else {
		delta.sbq_Low -= b->sbq_Low;
	}

	if (delta.sbq_High < b->sbq_High)
		goto out;

	/*delta.sbq_High -= b->sbq_High;*/

	result = delta.sbq_Low;

out:

	return(result);
}

typedef struct {
	SBQUAD_T in;
	SBQUAD_T out;
} Sample;

static Sample last_sample;

static BOOL get_counters(SBQUAD_T *in, SBQUAD_T *out)
{
	BOOL result = TRUE;

	if (SocketBaseTags(
		SBTM_GETREF(SBTC_GET_BYTES_RECEIVED), in,
		SBTM_GETREF(SBTC_GET_BYTES_SENT), out,
		TAG_END)) {

		printf("Could not query data throughput statistics.\n");
		result = FALSE;
	}

	return result;
}

void init_netstats(void)
{
	get_counters(&last_sample.in, &last_sample.out);
}

/*

Check out what has been going on through the network. Update parameter
values, and return TRUE if the graph data should be rescaled with the multiplier.

Recalculation should happen when a new peak in the transfers has happened.

*/
BOOL update_netstats(
	UBYTE *download, UBYTE *upload,
	float *dl_multiplier, float *ul_multiplier,
	float *dl_speed, float *ul_speed)
{
	static ULONG max_sent;
	static ULONG max_received;

	Sample sample;

	ULONG received, sent;

	BOOL redraw = FALSE;

	if (!get_counters(&sample.in, &sample.out)) {
			
		*upload = 0;
		*download = 0;
		*ul_speed = 0;
		*dl_speed = 0;
			
		return FALSE;
	}

	received = quad_delta(&sample.in, &last_sample.in);
	sent = quad_delta(&sample.out, &last_sample.out);

	last_sample = sample;

	if (sent > max_sent) {
		//printf("max_sent changes to %ld\n", sent);

		*ul_multiplier = (float) max_sent / sent;
		redraw = TRUE;
		max_sent = sent;
	}

	if (received > max_received) {
		//printf("max_received changes to %ld\n", received);

		*dl_multiplier = (float) max_received / received;
		redraw = TRUE;
		max_received = received;
	}

	// Scale the values to 0 ... 100
	if (max_sent > 0) {
		*upload = 100.f * sent / max_sent;
	} else {
		*upload = 0;
	}

	if (max_received > 0) {
		*download = 100.f * received / max_received;
	} else {
		*download = 0;
	}

	// Calculate current up/down load speeds in Kilobytes
	*ul_speed = sent / 1024.f;
	*dl_speed = received / 1024.f;

	//printf("s %d r %d m %f\n", *download, *upload, *multiplier);

	return redraw;
}

