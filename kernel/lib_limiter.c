// (c) 2012 Thomas Schoebel-Theuer / 1&1 Internet AG

#include "lib_limiter.h"

#include <linux/kernel.h>
#include <linux/module.h>

int xio_limit(struct xio_limiter *lim, int amount)
{
	int delay = 0;
	long long now;

	now = cpu_clock(raw_smp_processor_id());

	/* Compute the maximum delay along the path
	 * down to the root of the hierarchy tree.
	 */
	while (lim != NULL) {
		long long elapsed = now - lim->lim_stamp;
		/* Sometimes, raw CPU clocks may do weired things...
		 */
		if (unlikely(elapsed <= 0))
			elapsed = 1;

		/* Only use incremental accumulation at repeated calls, but
		 * never after longer pauses.
		 */
		if (likely(lim->lim_stamp && elapsed < LIMITER_TIME_RESOLUTION * 8)) {
			long long rate_raw;
			int rate;

			/* Races are possible, but taken into account.
			 * There is no real harm from rarely lost updates.
			 */
			if (likely(amount > 0))
				lim->lim_accu += amount;

			rate_raw = lim->lim_accu * LIMITER_TIME_RESOLUTION / elapsed;
			rate = rate_raw;
			if (unlikely(rate_raw > INT_MAX)) {
				rate = INT_MAX;
			}
			lim->lim_rate = rate;

			// limit exceeded?
			if (lim->lim_max_rate > 0 && rate > lim->lim_max_rate) {
				int this_delay = 1000 - (long long)lim->lim_max_rate * 1000 / rate;
				// compute maximum
				if (this_delay > delay)
					delay = this_delay;
			}

			elapsed -= LIMITER_TIME_RESOLUTION * 2;
			if (elapsed > LIMITER_TIME_RESOLUTION) {
				lim->lim_stamp += elapsed;
				lim->lim_accu -= (unsigned long long)lim->lim_rate * (unsigned long long)elapsed / LIMITER_TIME_RESOLUTION;
				if (lim->lim_accu < 0) {
					lim->lim_accu = 0;
				}
			}
		} else {
			if (unlikely(amount < 0))
				amount = 0;
			lim->lim_accu = amount;
			lim->lim_stamp = now;
			lim->lim_rate = 0;
		}
		lim = lim->lim_father;
	}
	return delay;
}
EXPORT_SYMBOL_GPL(xio_limit);
