/*
 *
 * Copyright 2021-2023 Software Radio Systems Limited
 *
 * This file is part of srsRAN.
 *
 * srsRAN is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * srsRAN is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * A copy of the GNU Affero General Public License can be found in
 * the LICENSE file in the top-level directory of this distribution
 * and at http://www.gnu.org/licenses/.
 *
 */

#include "ue_cell_grid_allocator.h"
#include "../support/dci_builder.h"
#include "../support/mcs_calculator.h"
#include "../support/mcs_tbs_calculator.h"
#include "../support/sch_pdu_builder.h"
#include "srsran/ran/pdcch/coreset.h"
#include "srsran/scheduler/scheduler_dci.h"
#include "srsran/support/error_handling.h"

using namespace srsran;

// Helpers that checks if the slot is a candidate one for CSI reporting for a given user.
static bool is_csi_slot(const serving_cell_config& ue_cfg, slot_point sl_tx)
{
  if (ue_cfg.csi_meas_cfg.has_value()) {
    // We assume we only use the first CSI report configuration.
    const unsigned csi_report_cfg_idx = 0;
    const auto&    csi_report_cfg     = ue_cfg.csi_meas_cfg.value().csi_report_cfg_list[csi_report_cfg_idx];

    // > Scheduler CSI grants.
    unsigned csi_offset =
        variant_get<csi_report_config::periodic_or_semi_persistent_report_on_pucch>(csi_report_cfg.report_cfg_type)
            .report_slot_offset;
    unsigned csi_period = csi_report_periodicity_to_uint(
        variant_get<csi_report_config::periodic_or_semi_persistent_report_on_pucch>(csi_report_cfg.report_cfg_type)
            .report_slot_period);

    if ((sl_tx - csi_offset).to_uint() % csi_period == 0) {
      return true;
    }
  }

  return false;
}

ue_cell_grid_allocator::ue_cell_grid_allocator(const scheduler_ue_expert_config& expert_cfg_,
                                               ue_repository&                    ues_,
                                               srslog::basic_logger&             logger_) :
  expert_cfg(expert_cfg_), ues(ues_), logger(logger_)
{
}

void ue_cell_grid_allocator::add_cell(du_cell_index_t           cell_index,
                                      pdcch_resource_allocator& pdcch_sched,
                                      uci_allocator&            uci_alloc,
                                      cell_resource_allocator&  cell_alloc)
{
  cells.emplace(cell_index, cell_t{cell_index, &pdcch_sched, &uci_alloc, &cell_alloc});
}

bool ue_cell_grid_allocator::allocate_dl_grant(const ue_pdsch_grant& grant)
{
  srsran_assert(ues.contains(grant.user->ue_index), "Invalid UE candidate index={}", grant.user->ue_index);
  srsran_assert(has_cell(grant.cell_index), "Invalid UE candidate cell_index={}", grant.cell_index);
  ue& u = ues[grant.user->ue_index];

  // Verify UE carrier is active.
  ue_cell* ue_cc = u.find_cell(grant.cell_index);
  if (ue_cc == nullptr or not ue_cc->is_active()) {
    logger.warning("PDSCH allocation failed. Cause: The ue={} carrier with cell_index={} is inactive",
                   u.ue_index,
                   grant.cell_index);
    return false;
  }

  const ue_cell_configuration& ue_cell_cfg = ue_cc->cfg();
  const cell_configuration&    cell_cfg    = ue_cell_cfg.cell_cfg_common;
  const bwp_downlink_common&   init_dl_bwp = *ue_cell_cfg.bwp(to_bwp_id(0)).dl_common;
  const bwp_downlink_common&   bwp_dl_cmn  = *ue_cell_cfg.bwp(ue_cc->active_bwp_id()).dl_common;
  dl_harq_process&             h_dl        = ue_cc->harqs.dl_harq(grant.h_id);

  // Find a SearchSpace candidate.
  const search_space_info* ss_info = ue_cell_cfg.find_search_space(grant.ss_id);
  if (ss_info == nullptr) {
    logger.warning("Failed to allocate PDSCH. Cause: No valid SearchSpace found.");
    return false;
  }
  if (ss_info->bwp->bwp_id != ue_cc->active_bwp_id()) {
    logger.warning("Failed to allocate PDSCH. Cause: SearchSpace not valid for active BWP.");
    return false;
  }
  const search_space_configuration& ss_cfg = *ss_info->cfg;

  // In case of re-transmission DCI format must remain same and therefore its necessary to find the SS which support
  // that DCI format.
  if (not h_dl.empty() and h_dl.last_alloc_params().dci_cfg_type != ss_info->get_crnti_dl_dci_format()) {
    return false;
  }

  const dci_dl_rnti_config_type dci_type = ss_info->get_crnti_dl_dci_format();

  // See 3GPP TS 38.213, clause 10.1,
  // A UE monitors PDCCH candidates in one or more of the following search spaces sets
  //  - a Type1-PDCCH CSS set configured by ra-SearchSpace in PDCCH-ConfigCommon for a DCI format with
  //    CRC scrambled by a RA-RNTI, a MsgB-RNTI, or a TC-RNTI on the primary cell.
  if (dci_type == dci_dl_rnti_config_type::tc_rnti_f1_0 and
      grant.ss_id != cell_cfg.dl_cfg_common.init_dl_bwp.pdcch_common.ra_search_space_id) {
    logger.debug("Failed to allocate PDSCH. Cause: SearchSpace not valid for re-transmission of msg4.");
    return false;
  }

  const span<const pdsch_time_domain_resource_allocation> pdsch_list   = ss_info->pdsch_time_domain_list;
  const pdsch_time_domain_resource_allocation&            pdsch_td_cfg = pdsch_list[grant.time_res_index];

  // Fetch PDCCH and PDSCH resource grid allocators.
  cell_slot_resource_allocator& pdcch_alloc = get_res_alloc(grant.cell_index)[0];
  cell_slot_resource_allocator& pdsch_alloc = get_res_alloc(grant.cell_index)[pdsch_td_cfg.k0];

  if (not cell_cfg.is_dl_enabled(pdcch_alloc.slot)) {
    logger.warning("Failed to allocate PDSCH in slot={}. Cause: DL is not active in the PDCCH slot", pdsch_alloc.slot);
    return false;
  }
  if (not cell_cfg.is_dl_enabled(pdsch_alloc.slot)) {
    logger.warning("Failed to allocate PDSCH in slot={}. Cause: DL is not active in the PDSCH slot", pdsch_alloc.slot);
    return false;
  }

  // Verify there is space in PDSCH and PDCCH result lists for new allocations.
  if (pdsch_alloc.result.dl.ue_grants.full() or pdcch_alloc.result.dl.dl_pdcchs.full()) {
    logger.warning("Failed to allocate PDSCH. Cause: No space available in scheduler output list");
    return false;
  }

  // Verify CRBs fit in the chosen BWP.
  if (not ss_info->dl_crb_lims.contains(grant.crbs)) {
    logger.warning(
        "Failed to allocate PDSCH. Cause: CRBs={} are outside the valid limits={}.", grant.crbs, ss_info->dl_crb_lims);
    return false;
  }

  // In case of retx, ensure the number of PRBs for the grant did not change.
  if (not h_dl.empty() and grant.crbs.length() != h_dl.last_alloc_params().rbs.type1().length()) {
    logger.warning("Failed to allocate PDSCH. Cause: Number of CRBs has to remain constant during retxs (Harq-id={}, "
                   "nof_prbs={}!={})",
                   h_dl.id,
                   h_dl.last_alloc_params().rbs.type1().length(),
                   grant.crbs.length());
    return false;
  }

  // Verify there is no RB collision.
  if (pdsch_alloc.dl_res_grid.collides(bwp_dl_cmn.generic_params.scs, pdsch_td_cfg.symbols, grant.crbs)) {
    logger.warning("Failed to allocate PDSCH. Cause: No space available in scheduler RB resource grid.");
    return false;
  }

  // Allocate PDCCH position.
  pdcch_dl_information* pdcch =
      get_pdcch_sched(grant.cell_index).alloc_dl_pdcch_ue(pdcch_alloc, u.crnti, ue_cell_cfg, ss_cfg.id, grant.aggr_lvl);
  if (pdcch == nullptr) {
    logger.info("Failed to allocate PDSCH. Cause: No space in PDCCH.");
    return false;
  }

  // Allocate UCI. UCI destination (i.e., PUCCH or PUSCH) depends on whether there exist a PUSCH grant for the UE.
  unsigned            k1      = 0;
  span<const uint8_t> k1_list = ss_info->get_k1_candidates();
  uci_allocation      uci     = {};
  // [Implementation-defined] We restrict the number of HARQ bits per PUCCH to 2, until the PUCCH allocator supports
  // more than this.
  const uint8_t max_harq_bits_per_uci = 2U;
  for (const uint8_t k1_candidate : k1_list) {
    const slot_point uci_slot = pdsch_alloc.slot + k1_candidate;
    if (not cell_cfg.is_fully_ul_enabled(uci_slot)) {
      continue;
    }
    // NOTE: This is only to avoid allocating more than 2 HARQ bits in PUCCH that are expected to carry CSI reporting.
    // TODO: Remove this when the PUCCH allocator handle properly more than 2 HARQ-ACK bits + CSI.
    if (is_csi_slot(u.get_pcell().cfg().cfg_dedicated(), uci_slot) and
        get_uci_alloc(grant.cell_index)
                .get_scheduled_pdsch_counter_in_ue_uci(get_res_alloc(grant.cell_index)[uci_slot], u.crnti) >=
            max_harq_bits_per_uci) {
      continue;
    }
    uci = get_uci_alloc(grant.cell_index)
              .alloc_uci_harq_ue(
                  get_res_alloc(grant.cell_index), u.crnti, u.get_pcell().cfg(), pdsch_td_cfg.k0, k1_candidate);
    if (uci.alloc_successful) {
      k1                                      = k1_candidate;
      pdcch->ctx.context.harq_feedback_timing = k1;
      break;
    }
  }
  if (not uci.alloc_successful) {
    logger.info("Failed to allocate PDSCH. Cause: No space in PUCCH.");
    get_pdcch_sched(grant.cell_index).cancel_last_pdcch(pdcch_alloc);
    return false;
  }

  pdsch_config_params pdsch_cfg;
  switch (dci_type) {
    case dci_dl_rnti_config_type::tc_rnti_f1_0:
      pdsch_cfg = get_pdsch_config_f1_0_tc_rnti(cell_cfg, pdsch_list[grant.time_res_index]);
      break;
    case dci_dl_rnti_config_type::c_rnti_f1_0:
      pdsch_cfg = get_pdsch_config_f1_0_c_rnti(ue_cell_cfg, pdsch_list[grant.time_res_index]);
      break;
    case dci_dl_rnti_config_type::c_rnti_f1_1:
      pdsch_cfg = get_pdsch_config_f1_1_c_rnti(ue_cell_cfg, pdsch_list[grant.time_res_index]);
      break;
    default:
      report_fatal_error("Unsupported PDCCH DCI UL format");
  }

  // Reduce estimated MCS by 1 whenever CSI-RS is sent over a particular slot to account for the overhead of CSI-RS REs.
  sch_mcs_index adjusted_mcs{grant.mcs};
  if (not pdsch_alloc.result.dl.csi_rs.empty()) {
    adjusted_mcs = adjusted_mcs == 0 ? adjusted_mcs : adjusted_mcs - 1;
  }

  optional<sch_mcs_tbs> mcs_tbs_info;
  // If it's a new Tx, compute the MCS and TBS.
  if (h_dl.empty()) {
    mcs_tbs_info = compute_dl_mcs_tbs(pdsch_cfg, ue_cell_cfg, adjusted_mcs, grant.crbs.length());
  } else {
    // It is a retx.
    mcs_tbs_info.emplace(
        sch_mcs_tbs{.mcs = h_dl.last_alloc_params().tb[0]->mcs, .tbs = h_dl.last_alloc_params().tb[0]->tbs_bytes});
  }

  // If there is not MCS-TBS info, it means no MCS exists such that the effective code rate is <= 0.95.
  if (not mcs_tbs_info.has_value()) {
    logger.warning("Failed to allocate PDSCH. Cause: no MCS such that code rate <= 0.95.");
    get_pdcch_sched(grant.cell_index).cancel_last_pdcch(pdcch_alloc);
    return false;
  }

  // Mark resources as occupied in the ResourceGrid.
  pdsch_alloc.dl_res_grid.fill(grant_info{bwp_dl_cmn.generic_params.scs, pdsch_td_cfg.symbols, grant.crbs});

  // Allocate UE DL HARQ.
  if (h_dl.empty()) {
    // It is a new tx.
    // TODO: Compute total DAI when using DCI Format 1_1 if UE is configured with multiple serving cells.
    h_dl.new_tx(pdsch_alloc.slot, k1, expert_cfg.max_nof_harq_retxs, uci.dai);
  } else {
    // It is a retx.
    h_dl.new_retx(pdsch_alloc.slot, k1, uci.dai);
  }

  // Fill DL PDCCH DCI PDU.
  uint8_t rv = ue_cc->get_pdsch_rv(h_dl);
  switch (dci_type) {
    case dci_dl_rnti_config_type::tc_rnti_f1_0:
      build_dci_f1_0_tc_rnti(pdcch->dci,
                             init_dl_bwp,
                             grant.crbs,
                             grant.time_res_index,
                             k1,
                             uci.pucch_grant.pucch_res_indicator,
                             mcs_tbs_info.value().mcs,
                             rv,
                             h_dl);
      break;
    case dci_dl_rnti_config_type::c_rnti_f1_0:
      build_dci_f1_0_c_rnti(pdcch->dci,
                            ue_cell_cfg,
                            grant.ss_id,
                            grant.crbs,
                            grant.time_res_index,
                            k1,
                            uci.pucch_grant.pucch_res_indicator,
                            uci.dai,
                            mcs_tbs_info.value().mcs,
                            rv,
                            h_dl);
      break;
    case dci_dl_rnti_config_type::c_rnti_f1_1:
      build_dci_f1_1_c_rnti(pdcch->dci,
                            ue_cell_cfg,
                            grant.ss_id,
                            crb_to_prb(ss_info->dl_crb_lims, grant.crbs),
                            grant.time_res_index,
                            k1,
                            uci.pucch_grant.pucch_res_indicator,
                            uci.dai,
                            mcs_tbs_info.value().mcs,
                            rv,
                            h_dl);
      break;
    default:
      report_fatal_error("Unsupported RNTI type for PDSCH allocation");
  }

  // Fill PDSCH PDU.
  dl_msg_alloc& msg     = pdsch_alloc.result.dl.ue_grants.emplace_back();
  msg.context.ue_index  = u.ue_index;
  msg.context.k1        = k1;
  msg.context.ss_id     = ss_cfg.id;
  msg.context.nof_retxs = h_dl.tb(0).nof_retxs;
  switch (pdcch->dci.type) {
    case dci_dl_rnti_config_type::tc_rnti_f1_0:
      build_pdsch_f1_0_tc_rnti(msg.pdsch_cfg,
                               pdsch_cfg,
                               mcs_tbs_info.value().tbs,
                               u.crnti,
                               cell_cfg,
                               pdcch->dci.tc_rnti_f1_0,
                               grant.crbs,
                               h_dl.tb(0).nof_retxs == 0);
      break;
    case dci_dl_rnti_config_type::c_rnti_f1_0:
      build_pdsch_f1_0_c_rnti(msg.pdsch_cfg,
                              pdsch_cfg,
                              mcs_tbs_info.value().tbs,
                              u.crnti,
                              ue_cell_cfg,
                              grant.ss_id,
                              pdcch->dci.c_rnti_f1_0,
                              grant.crbs,
                              h_dl.tb(0).nof_retxs == 0);
      break;
    case dci_dl_rnti_config_type::c_rnti_f1_1:
      build_pdsch_f1_1_c_rnti(msg.pdsch_cfg,
                              pdsch_cfg,
                              mcs_tbs_info.value(),
                              u.crnti,
                              ue_cell_cfg,
                              grant.ss_id,
                              pdcch->dci.c_rnti_f1_1,
                              grant.crbs,
                              h_dl);
      break;
    default:
      report_fatal_error("Unsupported PDCCH DL DCI format");
  }

  // Save set PDCCH and PDSCH PDU parameters in HARQ process.
  h_dl.save_alloc_params(pdcch->dci.type, msg.pdsch_cfg);

  if (h_dl.tb(0).nof_retxs == 0) {
    // Set MAC logical channels to schedule in this PDU if it is a newtx.
    u.build_dl_transport_block_info(msg.tb_list.emplace_back(), msg.pdsch_cfg.codewords[0].tb_size_bytes);
  }

  return true;
}

bool ue_cell_grid_allocator::allocate_ul_grant(const ue_pusch_grant& grant)
{
  srsran_assert(ues.contains(grant.user->ue_index), "Invalid UE candidate index={}", grant.user->ue_index);
  srsran_assert(has_cell(grant.cell_index), "Invalid UE candidate cell_index={}", grant.cell_index);
  constexpr static unsigned pdcch_delay_in_slots = 0;

  ue& u = ues[grant.user->ue_index];

  // Verify UE carrier is active.
  ue_cell* ue_cc = u.find_cell(grant.cell_index);
  if (ue_cc == nullptr or not ue_cc->is_active()) {
    logger.warning("PUSCH allocation failed. Cause: The ue={} carrier with cell_index={} is inactive",
                   u.ue_index,
                   grant.cell_index);
    return false;
  }

  const ue_cell_configuration& ue_cell_cfg = ue_cc->cfg();
  const cell_configuration&    cell_cfg    = ue_cell_cfg.cell_cfg_common;
  ul_harq_process&             h_ul        = ue_cc->harqs.ul_harq(grant.h_id);

  // Find a SearchSpace candidate.
  const search_space_info* ss_info = ue_cell_cfg.find_search_space(grant.ss_id);
  if (ss_info == nullptr) {
    logger.warning("Failed to allocate PUSCH. Cause: No valid SearchSpace found.");
    return false;
  }
  if (ss_info->bwp->bwp_id != ue_cc->active_bwp_id()) {
    logger.warning("Failed to allocate PUSCH. Cause: Chosen SearchSpace {} does not belong to the active BWP {}",
                   grant.ss_id,
                   ue_cc->active_bwp_id());
    return false;
  }
  const search_space_configuration&            ss_cfg       = *ss_info->cfg;
  const bwp_uplink_common&                     bwp_ul_cmn   = *ss_info->bwp->ul_common;
  dci_ul_rnti_config_type                      dci_type     = ss_info->get_crnti_ul_dci_format();
  const subcarrier_spacing                     scs          = bwp_ul_cmn.generic_params.scs;
  const pusch_time_domain_resource_allocation& pusch_td_cfg = ss_info->pusch_time_domain_list[grant.time_res_index];

  // In case of retx, verify whether DCI format match the DCI format supported by SearchSpace.
  if (not h_ul.empty()) {
    if (h_ul.last_tx_params().dci_cfg_type != dci_type) {
      logger.info("Failed to allocate PUSCH. Cause: DCI format {} in HARQ retx is not supported in SearchSpace {}.",
                  dci_ul_rnti_config_format(h_ul.last_tx_params().dci_cfg_type),
                  grant.ss_id);
      return false;
    }
    dci_type = h_ul.last_tx_params().dci_cfg_type;
  }

  // Fetch PDCCH and PDSCH resource grid allocators.
  cell_slot_resource_allocator& pdcch_alloc = get_res_alloc(grant.cell_index)[pdcch_delay_in_slots];
  cell_slot_resource_allocator& pusch_alloc = get_res_alloc(grant.cell_index)[pdcch_delay_in_slots + pusch_td_cfg.k2];

  if (not cell_cfg.is_dl_enabled(pdcch_alloc.slot)) {
    logger.warning("Failed to allocate PUSCH in slot={}. Cause: DL is not active in the PDCCH slot", pusch_alloc.slot);
    return false;
  }
  if (not cell_cfg.is_ul_enabled(pusch_alloc.slot)) {
    logger.warning("Failed to allocate PUSCH in slot={}. Cause: UL is not active in the PUSCH slot (k2={})",
                   pusch_alloc.slot,
                   pusch_td_cfg.k2);
    return false;
  }

  // Verify there is space in PUSCH and PDCCH result lists for new allocations.
  if (pusch_alloc.result.ul.puschs.full() or pdcch_alloc.result.dl.dl_pdcchs.full()) {
    logger.warning("Failed to allocate PUSCH in slot={}. Cause: No space available in scheduler output list",
                   pusch_alloc.slot);
    return false;
  }

  // Verify CRBs allocation.
  if (not ss_info->ul_crb_lims.contains(grant.crbs)) {
    logger.warning("Failed to allocate PUSCH. Cause: CRBs allocated outside the BWP.",
                   grant.crbs.length(),
                   ss_info->ul_crb_lims.length());
    return false;
  }

  // In case of retx, ensure the number of PRBs for the grant did not change.
  if (not h_ul.empty() and grant.crbs.length() != h_ul.last_tx_params().rbs.type1().length()) {
    logger.warning("Failed to allocate PUSCH. Cause: Number of CRBs has to remain constant during retxs (harq-id={}, "
                   "nof_prbs={}!={})",
                   h_ul.id,
                   h_ul.last_tx_params().rbs.type1().length(),
                   grant.crbs.length());
    return false;
  }

  // Verify there is no RB collision.
  if (pusch_alloc.ul_res_grid.collides(scs, pusch_td_cfg.symbols, grant.crbs)) {
    logger.warning("Failed to allocate PUSCH. Cause: No space available in scheduler RB resource grid.");
    return false;
  }

  // Allocate PDCCH position.
  pdcch_ul_information* pdcch =
      get_pdcch_sched(grant.cell_index).alloc_ul_pdcch_ue(pdcch_alloc, u.crnti, ue_cell_cfg, ss_cfg.id, grant.aggr_lvl);
  if (pdcch == nullptr) {
    logger.info("Failed to allocate PUSCH. Cause: No space in PDCCH.");
    return false;
  }

  // Fetch PUSCH parameters based on type of transmission.
  pusch_config_params pusch_cfg;
  switch (ss_info->get_crnti_ul_dci_format()) {
    case dci_ul_rnti_config_type::tc_rnti_f0_0:
      pusch_cfg = get_pusch_config_f0_0_tc_rnti(cell_cfg, pusch_td_cfg);
      break;
    case dci_ul_rnti_config_type::c_rnti_f0_0:
      pusch_cfg = get_pusch_config_f0_0_c_rnti(ue_cell_cfg, bwp_ul_cmn, pusch_td_cfg);
      break;
    case dci_ul_rnti_config_type::c_rnti_f0_1:
      pusch_cfg = get_pusch_config_f0_1_c_rnti(ue_cell_cfg, pusch_td_cfg);
      break;
    default:
      report_fatal_error("Unsupported PDCCH DCI UL format");
  }

  // Compute MCS and TBS for this transmission.
  optional<sch_mcs_tbs> mcs_tbs_info;
  // If it's a new Tx, compute the MCS and TBS from SNR, payload size, and available RBs.
  if (h_ul.empty()) {
    mcs_tbs_info = compute_ul_mcs_tbs(pusch_cfg, ue_cell_cfg, grant.mcs, grant.crbs.length());
  }
  // If it's a reTx, fetch the MCS and TBS from the previous transmission.
  else {
    mcs_tbs_info.emplace(sch_mcs_tbs{.mcs = h_ul.last_tx_params().mcs, .tbs = h_ul.last_tx_params().tbs_bytes});
  }

  // If there is not MCS-TBS info, it means no MCS exists such that the effective code rate is <= 0.95.
  if (not mcs_tbs_info.has_value()) {
    logger.warning("Failed to allocate PUSCH. Cause: no MCS such that code rate <= 0.95.");
    get_pdcch_sched(grant.cell_index).cancel_last_pdcch(pdcch_alloc);
    return false;
  }

  // Mark resources as occupied in the ResourceGrid.
  pusch_alloc.ul_res_grid.fill(grant_info{scs, pusch_td_cfg.symbols, grant.crbs});

  // Allocate UE UL HARQ.
  if (h_ul.empty()) {
    // It is a new tx.
    h_ul.new_tx(pusch_alloc.slot, expert_cfg.max_nof_harq_retxs);
  } else {
    // It is a retx.
    h_ul.new_retx(pusch_alloc.slot);
  }

  // Compute total DAI. See TS 38.213, 9.1.3.2.
  // Total DAI provides total number of transmissions at the end of each interval (slot in a cell). Values range from 1
  // to 4.
  // If a UE is not provided PDSCH-CodeBlockGroupTransmission and the UE is scheduled for a PUSCH transmission by
  // DCI format 0_1 with DAI field value V_T_DAI_UL = 4 and the UE has not received any PDCCH within the monitoring
  // occasions for PDCCH with DCI format 1_0 or DCI format 1_1 for scheduling PDSCH receptions or SPS PDSCH
  // release on any serving cell c and the UE does not have HARQ-ACK information in response to a SPS PDSCH
  // reception to multiplex in the PUSCH, the UE does not multiplex HARQ-ACK information in the PUSCH transmission.
  // NOTE: DAI is encoded as per left most column in Table 9.1.3-2 of TS 38.213.
  unsigned dai = 3;
  if (dci_type == dci_ul_rnti_config_type::c_rnti_f0_1) {
    unsigned total_harq_ack_in_uci = 0;
    for (unsigned cell_idx = 0; cell_idx < u.nof_cells(); cell_idx++) {
      const ue_cell& ue_cell_info = u.get_cell(static_cast<ue_cell_index_t>(cell_idx));
      total_harq_ack_in_uci +=
          get_uci_alloc(ue_cell_info.cell_index).get_scheduled_pdsch_counter_in_ue_uci(pusch_alloc, u.crnti);
    }
    if (total_harq_ack_in_uci != 0) {
      // See TS 38.213, Table 9.1.3-2. dai value below maps to the leftmost column in the table.
      dai = ((total_harq_ack_in_uci - 1) % 4);
    }
  }

  // Fill UL PDCCH DCI.
  uint8_t rv = ue_cc->get_pusch_rv(h_ul);
  switch (dci_type) {
    case dci_ul_rnti_config_type::tc_rnti_f0_0:
      build_dci_f0_0_tc_rnti(pdcch->dci,
                             *ue_cell_cfg.bwp(to_bwp_id(0)).dl_common,
                             ue_cell_cfg.bwp(ue_cc->active_bwp_id()).ul_common->generic_params,
                             grant.crbs,
                             grant.time_res_index,
                             mcs_tbs_info.value().mcs,
                             rv,
                             h_ul);
      break;
    case dci_ul_rnti_config_type::c_rnti_f0_0:
      build_dci_f0_0_c_rnti(
          pdcch->dci, ue_cell_cfg, grant.ss_id, grant.crbs, grant.time_res_index, mcs_tbs_info.value().mcs, rv, h_ul);
      break;
    case dci_ul_rnti_config_type::c_rnti_f0_1:
      build_dci_f0_1_c_rnti(pdcch->dci,
                            ue_cell_cfg,
                            grant.ss_id,
                            grant.crbs,
                            grant.time_res_index,
                            mcs_tbs_info.value().mcs,
                            rv,
                            h_ul,
                            dai);
      break;
    default:
      report_fatal_error("Unsupported PDCCH UL DCI format");
  }

  // Fill PUSCH.
  ul_sched_info& msg    = pusch_alloc.result.ul.puschs.emplace_back();
  msg.context.ue_index  = u.ue_index;
  msg.context.ss_id     = ss_cfg.id;
  msg.context.k2        = pusch_td_cfg.k2;
  msg.context.nof_retxs = h_ul.tb().nof_retxs;
  switch (pdcch->dci.type) {
    case dci_ul_rnti_config_type::tc_rnti_f0_0:
      build_pusch_f0_0_tc_rnti(msg.pusch_cfg,
                               pusch_cfg,
                               mcs_tbs_info.value().tbs,
                               u.crnti,
                               cell_cfg,
                               pdcch->dci.tc_rnti_f0_0,
                               grant.crbs,
                               h_ul.tb().nof_retxs == 0);
      break;
    case dci_ul_rnti_config_type::c_rnti_f0_0:
      build_pusch_f0_0_c_rnti(msg.pusch_cfg,
                              u.crnti,
                              pusch_cfg,
                              mcs_tbs_info.value().tbs,
                              cell_cfg,
                              bwp_ul_cmn,
                              pdcch->dci.c_rnti_f0_0,
                              grant.crbs,
                              h_ul.tb().nof_retxs == 0);
      break;
    case dci_ul_rnti_config_type::c_rnti_f0_1:
      build_pusch_f0_1_c_rnti(msg.pusch_cfg,
                              u.crnti,
                              pusch_cfg,
                              mcs_tbs_info.value(),
                              ue_cell_cfg,
                              ss_cfg.id,
                              pdcch->dci.c_rnti_f0_1,
                              grant.crbs,
                              h_ul);
      break;
    default:
      report_fatal_error("Unsupported PDCCH UL DCI format");
  }

  // Check if there is any UCI grant allocated on the PUCCH that can be moved to the PUSCH.
  get_uci_alloc(grant.cell_index).multiplex_uci_on_pusch(msg, pusch_alloc, ue_cell_cfg, u.crnti);

  // Save set PDCCH and PUSCH PDU parameters in HARQ process.
  h_ul.save_alloc_params(pdcch->dci.type, msg.pusch_cfg);

  // In case there is a SR pending. Reset it.
  u.reset_sr_indication();

  return true;
}
