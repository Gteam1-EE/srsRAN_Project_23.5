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

#include "ue_event_manager.h"
#include "../logging/scheduler_event_logger.h"
#include "../logging/scheduler_metrics_handler.h"

using namespace srsran;

ue_event_manager::ue_event_manager(const scheduler_ue_expert_config& expert_cfg_,
                                   ue_repository&                    ue_db_,
                                   sched_configuration_notifier&     mac_notifier_,
                                   scheduler_metrics_handler&        metrics_handler_,
                                   scheduler_event_logger&           ev_logger_) :
  expert_cfg(expert_cfg_),
  ue_db(ue_db_),
  mac_notifier(mac_notifier_),
  metrics_handler(metrics_handler_),
  ev_logger(ev_logger_),
  logger(srslog::fetch_basic_logger("SCHED"))
{
}

void ue_event_manager::handle_ue_creation_request(const sched_ue_creation_request_message& ue_request)
{
  // Create UE object outside the scheduler slot indication handler to minimize latency.
  std::unique_ptr<ue> u = std::make_unique<ue>(
      expert_cfg, *du_cells[ue_request.cfg.cells[0].serv_cell_cfg.cell_index].cfg, ue_request, metrics_handler);

  // Defer UE object addition to ue list to the slot indication handler.
  common_events.emplace(MAX_NOF_DU_UES, [this, u = std::move(u)]() mutable {
    // Insert UE in UE repository.
    du_ue_index_t   ueidx       = u->ue_index;
    rnti_t          rnti        = u->crnti;
    du_cell_index_t pcell_index = u->get_pcell().cell_index;
    ue_db.add_ue(std::move(u));
    auto& inserted_ue = ue_db[ueidx];

    // Log Event.
    ev_logger.enqueue(scheduler_event_logger::ue_creation_event{ueidx, rnti, pcell_index});

    // Notify metrics handler.
    metrics_handler.handle_ue_creation(
        inserted_ue.ue_index, inserted_ue.crnti, inserted_ue.get_pcell().cfg().cell_cfg_common.pci);

    // Notify Scheduler UE configuration is complete.
    mac_notifier.on_ue_config_complete(ueidx);
  });
}

void ue_event_manager::handle_ue_reconfiguration_request(const sched_ue_reconfiguration_message& ue_request)
{
  common_events.emplace(ue_request.ue_index, [this, ue_request]() {
    if (not ue_db.contains(ue_request.ue_index)) {
      log_invalid_ue_index(ue_request.ue_index, "UE Reconfig Request");
      return;
    }
    // Configure existing UE.
    ue_db[ue_request.ue_index].handle_reconfiguration_request(ue_request);

    // Log event.
    ev_logger.enqueue(ue_request);

    // Notify Scheduler UE configuration is complete.
    mac_notifier.on_ue_config_complete(ue_request.ue_index);
  });
}

void ue_event_manager::handle_ue_removal_request(du_ue_index_t ue_index)
{
  common_events.emplace(ue_index, [this, ue_index]() {
    if (not ue_db.contains(ue_index)) {
      logger.warning("Received request to delete ue={} that does not exist", ue_index);
      return;
    }
    rnti_t rnti = ue_db[ue_index].crnti;

    // Scheduler UE removal from repository.
    ue_db.schedule_ue_rem(ue_index);

    // Log event.
    ev_logger.enqueue(sched_ue_delete_message{ue_index, rnti});

    // Notify metrics.
    metrics_handler.handle_ue_deletion(ue_index);
  });
}

void ue_event_manager::handle_ul_bsr_indication(const ul_bsr_indication_message& bsr_ind)
{
  srsran_sanity_check(cell_exists(bsr_ind.cell_index), "Invalid cell index");

  common_events.emplace(bsr_ind.ue_index, [this, bsr_ind]() {
    if (not ue_db.contains(bsr_ind.ue_index)) {
      log_invalid_ue_index(bsr_ind.ue_index, "BSR");
      return;
    }
    auto& u = ue_db[bsr_ind.ue_index];
    // Handle event.
    u.handle_bsr_indication(bsr_ind);

    // Log event.
    if (ev_logger.enabled()) {
      scheduler_event_logger::bsr_event event{};
      event.ue_index             = bsr_ind.ue_index;
      event.rnti                 = bsr_ind.crnti;
      event.type                 = bsr_ind.type;
      event.reported_lcgs        = bsr_ind.reported_lcgs;
      event.tot_ul_pending_bytes = units::bytes{u.pending_ul_newtx_bytes()};
      ev_logger.enqueue(event);
    }

    // Notify metrics handler.
    metrics_handler.handle_ul_bsr_indication(bsr_ind);
  });
}

void ue_event_manager::handle_crc_indication(const ul_crc_indication& crc_ind)
{
  srsran_assert(cell_exists(crc_ind.cell_index), "Invalid cell index");

  for (unsigned i = 0; i != crc_ind.crcs.size(); ++i) {
    cell_specific_events[crc_ind.cell_index].emplace(
        crc_ind.crcs[i].ue_index, [this, sl_rx = crc_ind.sl_rx, crc = crc_ind.crcs[i]](ue_cell& ue_cc) {
          const int tbs = ue_cc.handle_crc_pdu(sl_rx, crc);
          if (tbs < 0) {
            return;
          }

          // Log event.
          ev_logger.enqueue(scheduler_event_logger::crc_event{
              crc.ue_index, crc.rnti, ue_cc.cell_index, sl_rx, crc.harq_id, crc.tb_crc_success, crc.ul_sinr_metric});

          // Notify metrics handler.
          metrics_handler.handle_crc_indication(crc, units::bytes{(unsigned)tbs});
        });
  }
}

void ue_event_manager::handle_harq_ind(ue_cell&                               ue_cc,
                                       slot_point                             uci_sl,
                                       span<const mac_harq_ack_report_status> harq_bits)
{
  static constexpr unsigned dai_mod = 4;

  for (unsigned harq_idx = 0; harq_idx != harq_bits.size(); ++harq_idx) {
    const mac_harq_ack_report_status ack_value = harq_bits[harq_idx];
    const dl_harq_process*           h_dl      = ue_cc.harqs.dl_ack_info(uci_sl, ack_value, harq_idx % dai_mod);
    if (h_dl != nullptr) {
      const units::bytes tbs{h_dl->last_alloc_params().tb[0]->tbs_bytes};
      // Log Event.
      ev_logger.enqueue(scheduler_event_logger::harq_ack_event{
          ue_cc.ue_index, ue_cc.rnti(), ue_cc.cell_index, uci_sl, h_dl->id, ack_value, tbs});

      if (ack_value != mac_harq_ack_report_status::dtx) {
        // Notify metric.
        metrics_handler.handle_dl_harq_ack(ue_cc.ue_index, ack_value == mac_harq_ack_report_status::ack, tbs);
      }
    }
  }
}

void ue_event_manager::handle_csi(ue_cell&                                                              ue_cc,
                                  const bounded_bitset<uci_constants::MAX_NOF_CSI_PART1_OR_PART2_BITS>& csi_bits)
{
  static const size_t cqi_payload_size = 4;
  if (csi_bits.size() < cqi_payload_size) {
    return;
  }
  // Refer to \ref mac_uci_pdu::pucch_f2_or_f3_or_f4_type::uci_payload_or_csi_information for the CSI payload bit
  // encoding.
  unsigned wb_cqi = (static_cast<unsigned>(csi_bits.test(0)) << 3) + (static_cast<unsigned>(csi_bits.test(1)) << 2) +
                    (static_cast<unsigned>(csi_bits.test(2)) << 1) + (static_cast<unsigned>(csi_bits.test(3)));

  // Forward CSI bits to UE.
  ue_cc.set_latest_wb_cqi(wb_cqi);

  // Log event.
  ev_logger.enqueue(scheduler_event_logger::csi_report_event{ue_cc.ue_index, ue_cc.rnti(), wb_cqi});

  // Report the CSI metric.
  metrics_handler.handle_csi_report(ue_cc.ue_index, csi_bits);
}

void ue_event_manager::handle_uci_indication(const uci_indication& ind)
{
  srsran_sanity_check(cell_exists(ind.cell_index), "Invalid cell index");

  for (unsigned i = 0; i != ind.ucis.size(); ++i) {
    const uci_indication::uci_pdu& uci = ind.ucis[i];

    cell_specific_events[ind.cell_index].emplace(
        uci.ue_index, [this, uci_sl = ind.slot_rx, uci_pdu = uci.pdu](ue_cell& ue_cc) {
          if (variant_holds_alternative<uci_indication::uci_pdu::uci_pucch_f0_or_f1_pdu>(uci_pdu)) {
            const auto& pdu = variant_get<uci_indication::uci_pdu::uci_pucch_f0_or_f1_pdu>(uci_pdu);

            // Process DL HARQ ACKs.
            if (not pdu.harqs.empty()) {
              handle_harq_ind(ue_cc, uci_sl, pdu.harqs);
            }

            // Process SRs.
            if (pdu.sr_detected) {
              // Handle SR indication.
              ue_db[ue_cc.ue_index].handle_sr_indication();

              // Log SR event.
              ev_logger.enqueue(scheduler_event_logger::sr_event{ue_cc.ue_index, ue_cc.rnti()});
            }

            // Report the PUCCH SINR metric.
            metrics_handler.handle_pucch_sinr(ue_cc.ue_index, pdu.ul_sinr);

          } else if (variant_holds_alternative<uci_indication::uci_pdu::uci_pusch_pdu>(uci_pdu)) {
            const auto& pdu = variant_get<uci_indication::uci_pdu::uci_pusch_pdu>(uci_pdu);

            // Process DL HARQ ACKs.
            if (pdu.harqs.size() > 0) {
              handle_harq_ind(ue_cc, uci_sl, pdu.harqs);
            }

            // Process CSI part 1 bits. NOTE: we assume there are only 4 bits, which represent the CQI.
            if (pdu.csi_part1.size() > 0) {
              handle_csi(ue_cc, pdu.csi_part1);
            }

          } else if (variant_holds_alternative<uci_indication::uci_pdu::uci_pucch_f2_or_f3_or_f4_pdu>(uci_pdu)) {
            const auto& pdu = variant_get<uci_indication::uci_pdu::uci_pucch_f2_or_f3_or_f4_pdu>(uci_pdu);

            // Process DL HARQ ACKs.
            if (pdu.harqs.size() > 0) {
              handle_harq_ind(ue_cc, uci_sl, pdu.harqs);
            }

            // Process SRs.
            const size_t sr_bit_position_with_1_sr_bit = 0;
            if (pdu.sr_info.size() > 0 and pdu.sr_info.test(sr_bit_position_with_1_sr_bit)) {
              // Handle SR indication.
              ue_db[ue_cc.ue_index].handle_sr_indication();

              // Log SR event.
              ev_logger.enqueue(scheduler_event_logger::sr_event{ue_cc.ue_index, ue_cc.rnti()});
            }

            // Process CSI part 1 bits. NOTE: we assume there are only 4 bits, which represent the CQI.
            if (pdu.csi_part1.size() > 0) {
              handle_csi(ue_cc, pdu.csi_part1);
            }

            // Report the PUCCH metric to the scheduler.
            metrics_handler.handle_pucch_sinr(ue_cc.ue_index, pdu.ul_sinr);
          }
        });
  }
}

void ue_event_manager::handle_dl_mac_ce_indication(const dl_mac_ce_indication& ce)
{
  common_events.emplace(ce.ue_index, [this, ce]() {
    if (not ue_db.contains(ce.ue_index)) {
      log_invalid_ue_index(ce.ue_index, "DL MAC CE");
      return;
    }
    ue_db[ce.ue_index].handle_dl_mac_ce_indication(ce);

    // Log event.
    ev_logger.enqueue(ce);
  });
}

void ue_event_manager::handle_dl_buffer_state_indication(const dl_buffer_state_indication_message& bs)
{
  common_events.emplace(bs.ue_index, [this, bs]() {
    if (not ue_db.contains(bs.ue_index)) {
      log_invalid_ue_index(bs.ue_index, "DL Buffer State");
      return;
    }
    ue& u = ue_db[bs.ue_index];

    u.handle_dl_buffer_state_indication(bs);
    if (bs.lcid == LCID_SRB0) {
      // Signal SRB0 scheduler with the new SRB0 buffer state.
      du_cells[u.get_pcell().cell_index].srb0_sched->handle_dl_buffer_state_indication(bs.ue_index);
    }

    // Log event.
    ev_logger.enqueue(bs);
  });
}

void ue_event_manager::process_common(slot_point sl, du_cell_index_t cell_index)
{
  if (last_sl != sl) {
    // Pop pending common events.
    common_events.slot_indication();
    last_sl = sl;
  }

  // Process events for UEs whose PCell matches cell_index argument.
  span<common_event_t> events_to_process = common_events.get_events();
  for (common_event_t& ev : events_to_process) {
    if (ev.callback.is_empty()) {
      // Event already processed.
      continue;
    }
    if (ev.ue_index == MAX_NOF_DU_UES) {
      // The UE is being created.
      ev.callback();
      ev.callback = {};
    } else {
      if (not ue_db.contains(ev.ue_index)) {
        // Can't find UE. Log error.
        log_invalid_ue_index(ev.ue_index);
        ev.callback = {};
        continue;
      }
      if (ue_db[ev.ue_index].get_pcell().cell_index == cell_index) {
        // If we are currently processing PCell.
        ev.callback();
        ev.callback = {};
      }
    }
  }
}

void ue_event_manager::process_cell_specific(du_cell_index_t cell_index)
{
  // Pop and process pending cell-specific events.
  cell_specific_events[cell_index].slot_indication();
  auto events = cell_specific_events[cell_index].get_events();
  for (cell_event_t& ev : events) {
    if (not ue_db.contains(ev.ue_index)) {
      log_invalid_ue_index(ev.ue_index);
      continue;
    }
    ue&      ue    = ue_db[ev.ue_index];
    ue_cell* ue_cc = ue.find_cell(cell_index);
    if (ue_cc == nullptr) {
      log_invalid_cc(ev.ue_index, cell_index);
      continue;
    }
    ev.callback(*ue_cc);
  }
}

void ue_event_manager::run(slot_point sl, du_cell_index_t cell_index)
{
  srsran_sanity_check(cell_exists(cell_index), "Invalid cell index {}", cell_index);

  // Process common events.
  process_common(sl, cell_index);

  // Process carrier specific events.
  process_cell_specific(cell_index);
}

void ue_event_manager::add_cell(const cell_configuration& cell_cfg_, ue_srb0_scheduler& srb0_sched)
{
  srsran_assert(not cell_exists(cell_cfg_.cell_index), "Overwriting cell configurations not supported");

  du_cells[cell_cfg_.cell_index].cfg        = &cell_cfg_;
  du_cells[cell_cfg_.cell_index].srb0_sched = &srb0_sched;
}

bool ue_event_manager::cell_exists(du_cell_index_t cell_index) const
{
  return cell_index < MAX_NOF_DU_CELLS and du_cells[cell_index].cfg != nullptr;
}

void ue_event_manager::log_invalid_ue_index(du_ue_index_t ue_index, const char* event_name) const
{
  logger.warning("{} for ue={} discarded. Cause: UE with provided Id does not exist", event_name, ue_index);
}

void ue_event_manager::log_invalid_cc(du_ue_index_t ue_index, du_cell_index_t cell_index) const
{
  logger.warning("Event for ue={} ignored. Cause: Cell {} is not configured.", ue_index, cell_index);
}
