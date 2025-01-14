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

#include "srsran/ofh/ecpri/ecpri_packet_decoder.h"
#include "srsran/srslog/logger.h"

namespace srsran {
namespace ecpri {

/// eCPRI packet decoder implementation.
class packet_decoder_impl : public packet_decoder
{
public:
  explicit packet_decoder_impl(srslog::basic_logger& logger_) : logger(logger_) {}

  // See interface for documentation.
  span<const uint8_t> decode(span<const uint8_t> packet, packet_parameters& params) override;

private:
  srslog::basic_logger& logger;
};

} // namespace ecpri
} // namespace srsran
