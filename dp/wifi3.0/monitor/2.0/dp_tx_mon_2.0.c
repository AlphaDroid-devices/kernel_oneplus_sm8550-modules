/*
 * Copyright (c) 2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2021,2022 Qualcomm Innovation Center, Inc. All rights reserved.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "hal_be_hw_headers.h"
#include "dp_types.h"
#include "hal_be_tx.h"
#include "hal_api.h"
#include "qdf_trace.h"
#include "hal_be_api_mon.h"
#include "dp_internal.h"
#include "qdf_mem.h"   /* qdf_mem_malloc,free */
#include "dp_mon.h"
#include <dp_mon_2.0.h>
#include <dp_tx_mon_2.0.h>
#include <dp_be.h>
#include <hal_be_api_mon.h>

static inline uint32_t
dp_tx_mon_srng_process_2_0(struct dp_soc *soc, struct dp_intr *int_ctx,
			   uint32_t mac_id, uint32_t quota)
{
	struct dp_pdev *pdev = dp_get_pdev_for_lmac_id(soc, mac_id);
	void *tx_mon_dst_ring_desc;
	hal_soc_handle_t hal_soc;
	void *mon_dst_srng;
	struct dp_mon_pdev *mon_pdev;
	uint32_t work_done = 0;
	struct dp_mon_soc *mon_soc = soc->monitor_soc;
	struct dp_mon_soc_be *mon_soc_be = dp_get_be_mon_soc_from_dp_mon_soc(mon_soc);
	union dp_mon_desc_list_elem_t *desc_list = NULL;
	union dp_mon_desc_list_elem_t *tail = NULL;
	struct dp_mon_desc_pool *tx_mon_desc_pool = &mon_soc_be->tx_desc_mon;

	if (!pdev) {
		dp_mon_err("%pK: pdev is null for mac_id = %d", soc, mac_id);
		return work_done;
	}

	mon_pdev = pdev->monitor_pdev;
	mon_dst_srng = mon_soc_be->tx_mon_dst_ring[mac_id].hal_srng;

	if (!mon_dst_srng || !hal_srng_initialized(mon_dst_srng)) {
		dp_mon_err("%pK: : HAL Monitor Destination Ring Init Failed -- %pK",
			   soc, mon_dst_srng);
		return work_done;
	}

	hal_soc = soc->hal_soc;

	qdf_assert((hal_soc && pdev));

	qdf_spin_lock_bh(&mon_pdev->mon_lock);

	if (qdf_unlikely(dp_srng_access_start(int_ctx, soc, mon_dst_srng))) {
		dp_mon_err("%s %d : HAL Mon Dest Ring access Failed -- %pK",
			   __func__, __LINE__, mon_dst_srng);
		qdf_spin_unlock_bh(&mon_pdev->mon_lock);
		return work_done;
	}

	while (qdf_likely((tx_mon_dst_ring_desc =
		(void *)hal_srng_dst_get_next(hal_soc, mon_dst_srng))
				&& quota--)) {
		struct hal_mon_desc hal_mon_tx_desc;
		struct dp_mon_desc *mon_desc;
		struct dp_mon_desc_pool *tx_desc_pool;

		tx_desc_pool = &mon_soc_be->tx_desc_mon;
		hal_be_get_mon_dest_status(soc->hal_soc,
					   tx_mon_dst_ring_desc,
					   &hal_mon_tx_desc);
		mon_desc = (struct dp_mon_desc *)(uintptr_t)(hal_mon_tx_desc.buf_addr);
		qdf_assert_always(mon_desc);

		if (!mon_desc->unmapped) {
			qdf_mem_unmap_page(soc->osdev, mon_desc->paddr,
					   DP_MON_DATA_BUFFER_SIZE,
					   QDF_DMA_FROM_DEVICE);
			mon_desc->unmapped = 1;
		}

		dp_tx_mon_process_status_tlv(soc, pdev,
					     &hal_mon_tx_desc,
					     mon_desc->paddr);

		qdf_frag_free(mon_desc->buf_addr);
		dp_mon_add_to_free_desc_list(&desc_list, &tail, mon_desc);
		work_done++;
	}
	dp_srng_access_end(int_ctx, soc, mon_dst_srng);

	if (desc_list)
		dp_mon_add_desc_list_to_free_list(soc, &desc_list,
						  &tail, tx_mon_desc_pool);

	qdf_spin_unlock_bh(&mon_pdev->mon_lock);
	dp_mon_info("mac_id: %d, work_done:%d", mac_id, work_done);
	return work_done;
}

uint32_t
dp_tx_mon_process_2_0(struct dp_soc *soc, struct dp_intr *int_ctx,
		      uint32_t mac_id, uint32_t quota)
{
	uint32_t work_done;

	work_done = dp_tx_mon_srng_process_2_0(soc, int_ctx, mac_id, quota);

	return work_done;
}

void
dp_tx_mon_buf_desc_pool_deinit(struct dp_soc *soc)
{
	struct dp_mon_soc *mon_soc = soc->monitor_soc;
	struct dp_mon_soc_be *mon_soc_be = dp_get_be_mon_soc_from_dp_mon_soc(mon_soc);

	dp_mon_desc_pool_deinit(&mon_soc_be->tx_desc_mon);
}

QDF_STATUS
dp_tx_mon_buf_desc_pool_init(struct dp_soc *soc)
{
	struct dp_mon_soc *mon_soc = soc->monitor_soc;
	struct dp_mon_soc_be *mon_soc_be = dp_get_be_mon_soc_from_dp_mon_soc(mon_soc);

	return dp_mon_desc_pool_init(&mon_soc_be->tx_desc_mon);
}

void dp_tx_mon_buf_desc_pool_free(struct dp_soc *soc)
{
	struct dp_mon_soc *mon_soc = soc->monitor_soc;
	struct dp_mon_soc_be *mon_soc_be = dp_get_be_mon_soc_from_dp_mon_soc(mon_soc);

	if (mon_soc_be)
		dp_mon_desc_pool_free(&mon_soc_be->tx_desc_mon);
}

QDF_STATUS
dp_tx_mon_buf_desc_pool_alloc(struct dp_soc *soc)
{
	struct dp_srng *mon_buf_ring;
	struct dp_mon_desc_pool *tx_mon_desc_pool;
	int entries;
	struct wlan_cfg_dp_soc_ctxt *soc_cfg_ctx;
	struct dp_mon_soc *mon_soc = soc->monitor_soc;
	struct dp_mon_soc_be *mon_soc_be = dp_get_be_mon_soc_from_dp_mon_soc(mon_soc);

	soc_cfg_ctx = soc->wlan_cfg_ctx;

	entries = wlan_cfg_get_dp_soc_tx_mon_buf_ring_size(soc_cfg_ctx);

	mon_buf_ring = &mon_soc_be->tx_mon_buf_ring;

	tx_mon_desc_pool = &mon_soc_be->tx_desc_mon;

	qdf_print("%s:%d tx mon buf desc pool entries: %d", __func__, __LINE__, entries);
	return dp_mon_desc_pool_alloc(entries, tx_mon_desc_pool);
}

void
dp_tx_mon_buffers_free(struct dp_soc *soc)
{
	struct dp_mon_desc_pool *tx_mon_desc_pool;
	struct dp_mon_soc *mon_soc = soc->monitor_soc;
	struct dp_mon_soc_be *mon_soc_be = dp_get_be_mon_soc_from_dp_mon_soc(mon_soc);

	tx_mon_desc_pool = &mon_soc_be->tx_desc_mon;

	dp_mon_pool_frag_unmap_and_free(soc, tx_mon_desc_pool);
}

QDF_STATUS
dp_tx_mon_buffers_alloc(struct dp_soc *soc, uint32_t size)
{
	struct dp_srng *mon_buf_ring;
	struct dp_mon_desc_pool *tx_mon_desc_pool;
	union dp_mon_desc_list_elem_t *desc_list = NULL;
	union dp_mon_desc_list_elem_t *tail = NULL;
	struct dp_mon_soc *mon_soc = soc->monitor_soc;
	struct dp_mon_soc_be *mon_soc_be = dp_get_be_mon_soc_from_dp_mon_soc(mon_soc);

	mon_buf_ring = &mon_soc_be->tx_mon_buf_ring;

	tx_mon_desc_pool = &mon_soc_be->tx_desc_mon;

	return dp_mon_buffers_replenish(soc, mon_buf_ring,
					tx_mon_desc_pool,
					size,
					&desc_list, &tail);
}
