/**
 * (C) Copyright 2019-2020 Intel Corporation.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * GOVERNMENT LICENSE RIGHTS-OPEN SOURCE SOFTWARE
 * The Government's rights to use, modify, reproduce, release, perform, display,
 * or disclose this software are subject to the terms of the Apache License as
 * provided in Contract No. B609815.
 * Any reproduction of computer software, computer software documentation, or
 * portions thereof marked with this legend must also reproduce the markings.
 */
/**
 * This file is part of daos
 *
 * vos/vos_ilog.c
 */
#define D_LOGFAC	DD_FAC(vos)

#include "vos_internal.h"

static int
vos_ilog_status_get(struct umem_instance *umm, umem_off_t tx_id,
		    uint32_t intent, void *args)
{
	int	rc;
	daos_handle_t coh;

	coh.cookie = (unsigned long)args;

	rc = vos_dtx_check_availability(umm, coh, tx_id, intent, DTX_RT_ILOG);
	if (rc < 0)
		return rc;

	switch (rc) {
	case ALB_UNAVAILABLE:
		return ILOG_UNCOMMITTED;
	case ALB_AVAILABLE_CLEAN:
		return ILOG_COMMITTED;
	case ALB_AVAILABLE_DIRTY:
		break;
	default:
		D_ASSERTF(0, "Unexpected availability\n");
	}

	return ILOG_REMOVED;
}

static int
vos_ilog_is_same_tx(struct umem_instance *umm, umem_off_t tx_id, bool *same,
		    void *args)
{
	umem_off_t dtx = vos_dtx_get();

	if (dtx == tx_id)
		*same = true;
	else
		*same = false;

	return 0;
}

static int
vos_ilog_add(struct umem_instance *umm, umem_off_t ilog_off, umem_off_t *tx_id,
	     void *args)
{
	return vos_dtx_register_record(umm, ilog_off, DTX_RT_ILOG, tx_id);
}

static int
vos_ilog_del(struct umem_instance *umm, umem_off_t ilog_off, umem_off_t tx_id,
	     void *args)
{
	daos_handle_t	coh;

	coh.cookie = (unsigned long)args;
	vos_dtx_deregister_record(umm, coh, tx_id, ilog_off);
	return 0;
}

void
vos_ilog_desc_cbs_init(struct ilog_desc_cbs *cbs, daos_handle_t coh)
{
	cbs->dc_log_status_cb	= vos_ilog_status_get;
	cbs->dc_log_status_args	= (void *)(unsigned long)coh.cookie;
	cbs->dc_is_same_tx_cb = vos_ilog_is_same_tx;
	cbs->dc_is_same_tx_args = NULL;
	cbs->dc_log_add_cb = vos_ilog_add;
	cbs->dc_log_add_args = NULL;
	cbs->dc_log_del_cb = vos_ilog_del;
	cbs->dc_log_del_args = (void *)(unsigned long)coh.cookie;
}

static void
vos_parse_ilog(struct vos_ilog_info *info, daos_epoch_t epoch,
	       daos_epoch_t punch, daos_epoch_t any_punch)
{
	struct ilog_entry	*entry;

	D_ASSERT(punch <= epoch);
	D_ASSERT(any_punch <= epoch);

	ilog_foreach_entry_reverse(&info->ii_entries, entry) {
		if (entry->ie_status == ILOG_REMOVED)
			continue;

		info->ii_empty = false;

		/** If a punch epoch is passed in, and it is later than any
		 * punch in this log, treat it as a prior punch
		 */
		if (entry->ie_id.id_epoch <= punch) {
			info->ii_prior_punch = punch;
			if (info->ii_prior_any_punch < punch)
				info->ii_prior_any_punch = punch;
			break;
		}

		if (entry->ie_id.id_epoch > epoch) {
			if (entry->ie_punch &&
			    entry->ie_status == ILOG_COMMITTED)
				info->ii_next_punch = entry->ie_id.id_epoch;
			continue;
		}

		if (entry->ie_punch &&
		    info->ii_prior_any_punch < entry->ie_id.id_epoch)
			info->ii_prior_any_punch = entry->ie_id.id_epoch;

		if (entry->ie_status == ILOG_UNCOMMITTED) {
			/** Key is not visible at current entry but may be yet
			 *  visible at prior entry
			 */
			if (info->ii_uncommitted < entry->ie_id.id_epoch &&
			    entry->ie_id.id_epoch > info->ii_create &&
			    entry->ie_id.id_epoch > info->ii_prior_punch)
				info->ii_uncommitted = entry->ie_id.id_epoch;
			continue;
		}

		/** We we have a committed entry that exceeds uncommitted
		 *  epoch, clear the uncommitted epoch.
		 */
		if (entry->ie_id.id_epoch > info->ii_uncommitted)
			info->ii_uncommitted = 0;

		D_ASSERT(entry->ie_status == ILOG_COMMITTED);

		if (entry->ie_punch) {
			info->ii_prior_punch = entry->ie_id.id_epoch;
			break;
		}

		info->ii_create = entry->ie_id.id_epoch;
	}

	if (punch > info->ii_prior_punch)
		info->ii_prior_punch = punch;
	if (punch > info->ii_prior_any_punch)
		info->ii_prior_any_punch = punch;

	D_DEBUG(DB_TRACE, "After fetch at "DF_U64": create="DF_U64
		" prior_punch="DF_U64" next_punch="DF_U64"%s\n", epoch,
		info->ii_create, info->ii_prior_punch, info->ii_next_punch,
		info->ii_empty ? " is empty" : "");
}

int
vos_ilog_fetch_(struct umem_instance *umm, daos_handle_t coh, uint32_t intent,
		struct ilog_df *ilog, daos_epoch_t epoch, daos_epoch_t punched,
		const struct vos_ilog_info *parent, struct vos_ilog_info *info)
{
	struct ilog_desc_cbs	 cbs;
	daos_epoch_t		 punch;
	daos_epoch_t		 punch_any;
	int			 rc;

	vos_ilog_desc_cbs_init(&cbs, coh);
	rc = ilog_fetch(umm, ilog, &cbs, intent, &info->ii_entries);
	if (rc == -DER_NONEXIST)
		goto init;
	if (rc != 0) {
		D_CDEBUG(rc == -DER_INPROGRESS, DB_IO, DLOG_ERR,
			 "Could not fetch ilog: "DF_RC"\n", DP_RC(rc));
		return rc;
	}

init:
	info->ii_uncommitted = 0;
	info->ii_create = 0;
	info->ii_next_punch = 0;
	info->ii_empty = true;
	info->ii_prior_punch = 0;
	info->ii_prior_any_punch = 0;
	punch = punched;
	punch_any = punched; /* Only matters if parent is passed in */
	if (parent != NULL) {
		punch = parent->ii_prior_punch;
		punch_any = parent->ii_prior_any_punch;
		info->ii_uncommitted = parent->ii_uncommitted;
	}

	if (rc == 0)
		vos_parse_ilog(info, epoch, punch, punch_any);

	return rc;
}

int
vos_ilog_check_(struct vos_ilog_info *info, const daos_epoch_range_t *epr_in,
		daos_epoch_range_t *epr_out, bool visible_only)
{
	if (epr_out && epr_out != epr_in)
		*epr_out = *epr_in;

	if (visible_only) {
		if (info->ii_create == 0)
			return -DER_NONEXIST;
		if (epr_out && epr_out->epr_lo < info->ii_create)
			epr_out->epr_lo = info->ii_create;
		return 0;
	}

	/* Caller wants to see punched entries so we will return 0 if either the
	 * entity is visible, has no incarnation log, or has a visible punch
	 */
	if (info->ii_empty) {
		/* mark whole thing as punched */
		info->ii_prior_punch = epr_in->epr_hi;
		return 0;
	}

	if (info->ii_create == 0) {
		if (info->ii_prior_punch == 0)
			return -DER_NONEXIST;
		/* Punch isn't in range so ignore it */
		if (info->ii_prior_punch < epr_in->epr_lo)
			return -DER_NONEXIST;
		return 0;
	}

	/* Ok, entity exists.  Punch fields will be set appropriately so caller
	 * can interpret them.
	 */

	return 0;
}

static inline int
vos_ilog_update_check(struct vos_ilog_info *info, const daos_epoch_range_t *epr)
{
	if (info->ii_create <= info->ii_prior_any_punch)
		return -DER_NONEXIST;

	return 0;
}

int vos_ilog_update_(struct vos_container *cont, struct ilog_df *ilog,
		     const daos_epoch_range_t *epr,
		     struct vos_ilog_info *parent, struct vos_ilog_info *info,
		     uint32_t cond, struct vos_ts_set *ts_set)
{
	daos_epoch_range_t	 max_epr = *epr;
	struct ilog_desc_cbs	 cbs;
	daos_handle_t		 loh;
	int			 rc;

	if (parent != NULL) {
		D_ASSERT(parent->ii_prior_any_punch >= parent->ii_prior_punch);

		if (parent->ii_prior_any_punch > max_epr.epr_lo)
			max_epr.epr_lo = parent->ii_prior_any_punch;
	}

	D_DEBUG(DB_TRACE, "Checking and updating incarnation log in range "
		DF_U64"-"DF_U64"\n", max_epr.epr_lo, max_epr.epr_hi);

	/** Do a fetch first.  The log may already exist */
	rc = vos_ilog_fetch(vos_cont2umm(cont), vos_cont2hdl(cont),
			    DAOS_INTENT_UPDATE, ilog, epr->epr_hi,
			    0, parent, info);
	/** For now, if the state isn't settled, just retry with later
	 *  timestamp.   The state should get settled quickly when there
	 *  is conditional update and sharing.
	 */
	if (cond == VOS_ILOG_COND_UPDATE && info->ii_uncommitted != 0)
		return -DER_INPROGRESS;
	if (rc == -DER_NONEXIST)
		goto update;
	if (rc != 0) {
		D_ERROR("Could not update ilog %p at "DF_U64": "DF_RC"\n",
			ilog, epr->epr_hi, DP_RC(rc));
		return rc;
	}

	rc = vos_ilog_update_check(info, &max_epr);
	if (rc == 0) {
		if (cond == VOS_ILOG_COND_INSERT)
			return -DER_EXIST;
		return rc;
	}
	if (rc != -DER_NONEXIST) {
		D_ERROR("Check failed: "DF_RC"\n", DP_RC(rc));
		return rc;
	}
update:
	if (rc == -DER_NONEXIST && cond == VOS_ILOG_COND_UPDATE)
		return -DER_NONEXIST;

	vos_ilog_desc_cbs_init(&cbs, vos_cont2hdl(cont));
	rc = ilog_open(vos_cont2umm(cont), ilog, &cbs, &loh);
	if (rc != 0) {
		D_ERROR("Could not open incarnation log: "DF_RC"\n", DP_RC(rc));
		return rc;
	}

	rc = ilog_update(loh, &max_epr, epr->epr_hi, false);

	ilog_close(loh);

	if (rc != 0) {
		D_ERROR("Could not update incarnation log: "DF_RC"\n",
			DP_RC(rc));
		return rc;
	}

	/* No need to refetch the log.  The only field that is used by update
	 * is prior_any_punch.   This field will not be changed by ilog_update
	 * for the purpose of parsing the child log.
	 */

	return rc;
}

int
vos_ilog_punch_(struct vos_container *cont, struct ilog_df *ilog,
		const daos_epoch_range_t *epr, struct vos_ilog_info *parent,
		struct vos_ilog_info *info, struct vos_ts_set *ts_set,
		bool leaf)
{
	daos_epoch_range_t	 max_epr = *epr;
	struct ilog_desc_cbs	 cbs;
	daos_handle_t		 loh;
	int			 rc;

	if (ts_set == NULL || (ts_set->ts_flags & VOS_OF_COND_PUNCH) == 0) {
		if (leaf)
			goto punch_log;
		return 0;
	}

	/** If we get here, we need to check if the entry exists */
	D_ASSERT(ts_set->ts_flags & VOS_OF_COND_PUNCH);

	if (parent != NULL) {
		D_ASSERT(parent->ii_prior_any_punch >= parent->ii_prior_punch);

		if (parent->ii_prior_any_punch > max_epr.epr_lo)
			max_epr.epr_lo = parent->ii_prior_any_punch;
	}

	D_DEBUG(DB_TRACE, "Checking existence of incarnation log in range "
		DF_U64"-"DF_U64"\n", max_epr.epr_lo, max_epr.epr_hi);

	/** Do a fetch first.  The log may already exist */
	rc = vos_ilog_fetch(vos_cont2umm(cont), vos_cont2hdl(cont),
			    DAOS_INTENT_PUNCH, ilog, epr->epr_hi,
			    0, parent, info);
	/** For now, if the state isn't settled, just retry with later
	 *  timestamp.   The state should get settled quickly when there
	 *  is conditional update and sharing.
	 */
	if (info->ii_uncommitted != 0)
		return -DER_INPROGRESS;
	if (rc == -DER_NONEXIST)
		return -DER_NONEXIST;
	if (rc != 0) {
		D_ERROR("Could not update ilog %p at "DF_U64": "DF_RC"\n",
			ilog, epr->epr_hi, DP_RC(rc));
		return rc;
	}

	rc = vos_ilog_update_check(info, &max_epr);
	if (rc == -DER_NONEXIST)
		return -DER_NONEXIST;
	if (rc != 0) {
		D_ERROR("Check failed: "DF_RC"\n", DP_RC(rc));
		return rc;
	}
	if (!leaf)
		return 0;

punch_log:
	vos_ilog_desc_cbs_init(&cbs, vos_cont2hdl(cont));
	rc = ilog_open(vos_cont2umm(cont), ilog, &cbs, &loh);
	if (rc != 0) {
		D_ERROR("Could not open incarnation log: "DF_RC"\n", DP_RC(rc));
		return rc;
	}

	rc = ilog_update(loh, NULL, epr->epr_hi, true);

	ilog_close(loh);

	if (rc != 0) {
		D_ERROR("Could not update incarnation log: "DF_RC"\n",
			DP_RC(rc));
		return rc;
	}

	return rc;
}

int
vos_ilog_aggregate(daos_handle_t coh, struct ilog_df *ilog,
		   const daos_epoch_range_t *epr,
		   bool discard, daos_epoch_t punched,
		   struct vos_ilog_info *info)
{
	struct vos_container	*cont = vos_hdl2cont(coh);
	struct umem_instance	*umm = vos_cont2umm(cont);
	struct ilog_desc_cbs	 cbs;
	int			 rc;

	vos_ilog_desc_cbs_init(&cbs, coh);
	D_DEBUG(DB_TRACE, "log="DF_X64"\n", umem_ptr2off(umm, ilog));

	rc = ilog_aggregate(umm, ilog, &cbs, epr, discard, punched,
			    &info->ii_entries);

	if (rc != 0)
		return rc;

	return vos_ilog_fetch(umm, coh, DAOS_INTENT_PURGE, ilog, epr->epr_hi,
			      punched, NULL, info);
}

void
vos_ilog_fetch_init(struct vos_ilog_info *info)
{
	memset(info, 0, sizeof(*info));
	ilog_fetch_init(&info->ii_entries);
}

/** Finalize incarnation log information */
void
vos_ilog_fetch_finish(struct vos_ilog_info *info)
{
	ilog_fetch_finish(&info->ii_entries);
}

int
vos_ilog_init(void)
{
	int	rc;

	rc = ilog_init();
	if (rc != 0) {
		D_ERROR("Failed to initialize incarnation log globals\n");
		return rc;
	}

	return 0;
}

bool
vos_ilog_ts_lookup(struct vos_ts_set *ts_set, struct ilog_df *ilog)
{
	struct vos_ts_entry	*entry;
	uint32_t		*idx;

	if (ts_set == NULL)
		return true;

	idx = ilog_ts_idx_get(ilog);

	return vos_ts_lookup(ts_set, idx, false, &entry);
}

int
vos_ilog_ts_cache(struct vos_ts_set *ts_set, struct ilog_df *ilog,
		  void *record, daos_size_t rec_size)
{
	struct vos_ts_entry	*entry;
	uint32_t		*idx;
	uint64_t		 hash;

	if (ts_set == NULL)
		return 0;

	hash = vos_hash_get(record, rec_size);
	if (ilog) {
		idx = ilog_ts_idx_get(ilog);
		entry = vos_ts_alloc(ts_set, idx, hash);
		if (entry == NULL)
			return -DER_NO_PERM;
	} else {
		vos_ts_get_negative(ts_set, hash, false);
	}

	return 0;
}

void
vos_ilog_ts_mark(struct vos_ts_set *ts_set, struct ilog_df *ilog)
{
	uint32_t		*idx = ilog_ts_idx_get(ilog);

	vos_ts_set_mark_entry(ts_set, idx);
}

void
vos_ilog_ts_evict(struct ilog_df *ilog, uint32_t type)
{
	uint32_t	*idx;

	idx = ilog_ts_idx_get(ilog);

	return vos_ts_evict(idx, type);
}
