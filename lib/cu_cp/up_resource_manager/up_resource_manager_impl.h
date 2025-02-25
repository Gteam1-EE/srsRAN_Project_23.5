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

#pragma once

#include "srsran/cu_cp/up_resource_manager.h"
#include <map>

namespace srsran {

namespace srs_cu_cp {

/// UP resource manager implementation
class up_resource_manager_impl final : public up_resource_manager
{
public:
  up_resource_manager_impl(const up_resource_manager_cfg& cfg);
  ~up_resource_manager_impl() = default;

  bool validate_request(const cu_cp_pdu_session_resource_setup_request& pdu) override;
  bool validate_request(const cu_cp_pdu_session_resource_modify_request& pdu) override;

  up_config_update calculate_update(const cu_cp_pdu_session_resource_setup_request& pdu) override;
  up_config_update calculate_update(const cu_cp_pdu_session_resource_modify_request& pdu) override;

  bool                          apply_config_update(const up_config_update_result& config) override;
  up_pdu_session_context        get_context(pdu_session_id_t psi) override;
  up_drb_context                get_context(drb_id_t drb_id) override;
  bool                          has_pdu_session(pdu_session_id_t pdu_session_id) override;
  size_t                        get_nof_drbs() override;
  size_t                        get_nof_pdu_sessions() override;
  std::vector<pdu_session_id_t> get_pdu_sessions() override;

private:
  up_drb_context get_drb(drb_id_t drb_id);
  bool           valid_5qi(const five_qi_t five_qi);

  up_resource_manager_cfg cfg;

  up_context context; // The currently active state.

  srslog::basic_logger& logger;
};

} // namespace srs_cu_cp

} // namespace srsran
