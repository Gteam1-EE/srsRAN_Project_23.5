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

#include "ofh_downlink_handler_impl.h"
#include "srsran/phy/support/resource_grid_context.h"

using namespace srsran;
using namespace ofh;

downlink_handler_impl::downlink_handler_impl(unsigned                                              eaxc_data_,
                                             std::unique_ptr<data_flow_cplane_scheduling_commands> data_flow_cplane_,
                                             std::unique_ptr<data_flow_uplane_downlink_data>       data_flow_uplane_) :
  eaxc_data(eaxc_data_), data_flow_cplane(std::move(data_flow_cplane_)), data_flow_uplane(std::move(data_flow_uplane_))
{
  srsran_assert(data_flow_cplane, "Invalid Control-Plane data flow");
  srsran_assert(data_flow_uplane, "Invalid Use-Plane data flow");
}

void downlink_handler_impl::handle_dl_data(const resource_grid_context& context, const resource_grid_reader& grid)
{
  // Control-Plane data flow.
  data_flow_cplane->enqueue_section_type_1_message(
      context.slot, eaxc_data, data_direction::downlink, filter_index_type::standard_channel_filter);

  // User-Plane data flow.
  data_flow_uplane->enqueue_section_type_1_message(context, grid, eaxc_data);
}
