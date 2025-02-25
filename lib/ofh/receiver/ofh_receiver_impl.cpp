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

#include "ofh_receiver_impl.h"
#include "srsran/ofh/ethernet/ethernet_properties.h"

using namespace srsran;
using namespace ofh;

/// Returns an Open Fronthaul uplink packet handler configuration from the given receiver implementation configuration.
static uplane_uplink_packet_handler_config get_packet_handler_config(const receiver_config&      config,
                                                                     receiver_impl_dependencies& depen)
{
  uplane_uplink_packet_handler_config out_cfg(*depen.logger, *depen.ul_cp_context_repo);
  out_cfg.is_prach_cp_enabled = config.is_prach_cp_enabled;
  out_cfg.ru_prach_port       = config.ru_prach_port;
  out_cfg.ru_ul_data_port     = config.ru_ul_data_port;
  out_cfg.uplane_decoder      = std::move(depen.uplane_decoder);
  out_cfg.ecpri_decoder       = std::move(depen.ecpri_decoder);
  out_cfg.eth_frame_decoder   = std::move(depen.eth_frame_decoder);

  // VLAN configuration.
  out_cfg.vlan_params.eth_type        = ether::ECPRI_ETH_TYPE;
  out_cfg.vlan_params.tci             = config.tci;
  out_cfg.vlan_params.mac_dst_address = config.mac_dst_address;
  out_cfg.vlan_params.mac_src_address = config.mac_src_address;

  return out_cfg;
}

/// Returns an Open Fronthaul User-Plane uplink symbol manager configuration from the given receiver implementation
/// configuration and handlers.
static uplane_uplink_symbol_manager_config
get_uplink_symbol_manager_config(const receiver_config&        config,
                                 receiver_impl_dependencies&   depen,
                                 uplane_uplink_packet_handler& packet_handler)
{
  uplane_uplink_symbol_manager_config out_cfg(
      *depen.logger, *depen.notifier, packet_handler, *depen.prach_context_repo, *depen.ul_slot_context_repo);

  out_cfg.du_ul_nof_prbs = config.du_ul_slot_nof_prbs;

  return out_cfg;
}

receiver_impl::receiver_impl(const receiver_config& config, receiver_impl_dependencies&& depen) :
  decompressor_sel(std::move(depen.decompressor_sel)),
  ul_packet_handler(get_packet_handler_config(config, depen)),
  ul_symbol_manager(get_uplink_symbol_manager_config(config, depen, ul_packet_handler)),
  ota_rx_handler(*depen.ul_cp_context_repo, *depen.prach_context_repo, *depen.ul_slot_context_repo)
{
}

ota_symbol_boundary_notifier& receiver_impl::get_ota_symbol_notifier()
{
  return ota_rx_handler;
}

ether::frame_notifier& receiver_impl::get_ethernet_frame_notifier()
{
  return ul_symbol_manager;
}
