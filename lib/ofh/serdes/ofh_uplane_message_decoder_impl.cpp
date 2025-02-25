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

#include "ofh_uplane_message_decoder_impl.h"
#include "../support/network_order_binary_deserializer.h"
#include "../support/ofh_uplane_constants.h"
#include "srsran/ofh/compression/iq_decompressor.h"
#include "srsran/support/units.h"

using namespace srsran;
using namespace ofh;

/// Number of bytes of the User-Plane header.
static constexpr unsigned NOF_BYTES_UP_HEADER = 4U;

/// Size in bytes of a section ID header with no compression.
static constexpr unsigned SECTION_ID_HEADER_NO_COMPRESSION_SIZE = 4U;

bool uplane_message_decoder_impl::decode(uplane_message_decoder_results& results, span<const uint8_t> message)
{
  network_order_binary_deserializer deserializer(message);

  // Decode the header.
  if (!decode_header(results, deserializer)) {
    return false;
  }

  // Decode the sections from the message.
  if (!decode_all_sections(results, deserializer)) {
    return false;
  }

  return true;
}

/// Checks the Open Fronthaul User-Plane header and returns true on success, otherwise false.
static bool is_header_valid(const uplane_message_params& params, srslog::basic_logger& logger, unsigned nof_symbols)
{
  if (params.direction != data_direction::uplink) {
    logger.debug("Dropping incoming Open Fronthaul message as it is not an uplink message");

    return false;
  }

  if (params.payload_version != OFH_PAYLOAD_VERSION) {
    logger.debug("Dropping incoming Open Fronthaul message as its payload version is {} but only {} is supported",
                 params.payload_version,
                 OFH_PAYLOAD_VERSION);

    return false;
  }

  if (params.filter_index == filter_index_type::reserved) {
    logger.debug("Dropping incoming Open Fronthaul message as its filter index is a reserved value {}",
                 params.filter_index);

    return false;
  }

  if (params.symbol_id >= nof_symbols) {
    logger.debug(
        "Dropping incoming Open Fronthaul message as its symbol index is {} and this decoder supports up to {} symbols",
        params.symbol_id,
        nof_symbols);

    return false;
  }

  return true;
}

bool uplane_message_decoder_impl::decode_header(uplane_message_decoder_results&    results,
                                                network_order_binary_deserializer& deserializer)
{
  if (deserializer.remaining_bytes() < NOF_BYTES_UP_HEADER) {
    logger.debug(
        "Dropping incoming Open Fronthaul message as its size is {} and it is smaller than the message header size.",
        deserializer.remaining_bytes());

    return false;
  }

  uplane_message_params& params = results.params;

  uint8_t value          = deserializer.read<uint8_t>();
  params.direction       = static_cast<data_direction>(value >> 7);
  params.payload_version = (value >> 4) & 7;
  params.filter_index    = to_filter_index_type(value & 0xf);

  // Slot.
  uint8_t  frame             = deserializer.read<uint8_t>();
  uint8_t  subframe_and_slot = deserializer.read<uint8_t>();
  uint8_t  subframe          = subframe_and_slot >> 4;
  unsigned slot_id           = 0;
  slot_id |= (subframe_and_slot & 0x0f) << 2;

  uint8_t slot_and_symbol = deserializer.read<uint8_t>();
  params.symbol_id        = slot_and_symbol & 0x3f;
  slot_id |= slot_and_symbol >> 6;

  params.slot = slot_point(to_numerology_value(scs), frame, subframe, slot_id);

  return is_header_valid(params, logger, nof_symbols);
}

bool uplane_message_decoder_impl::decode_all_sections(uplane_message_decoder_results&    results,
                                                      network_order_binary_deserializer& deserializer)
{
  // Decode sections while the message has remaining bytes.
  while (deserializer.remaining_bytes()) {
    // Try to decode section.
    if (!decode_section(results, deserializer)) {
      break;
    }
  }

  return !results.sections.empty();
}

bool uplane_message_decoder_impl::decode_section(uplane_message_decoder_results&    results,
                                                 network_order_binary_deserializer& deserializer)
{
  // Add a section to the results.
  uplane_section_params ofh_up_section;

  if (!decode_section_header(ofh_up_section, deserializer)) {
    return false;
  }

  if (!decode_compression_header(ofh_up_section, deserializer)) {
    return false;
  }

  if (!decode_iq_data(ofh_up_section, deserializer)) {
    return false;
  }

  results.sections.emplace_back(ofh_up_section);

  return true;
}

bool uplane_message_decoder_impl::decode_section_header(uplane_section_params&             results,
                                                        network_order_binary_deserializer& deserializer)
{
  if (deserializer.remaining_bytes() < SECTION_ID_HEADER_NO_COMPRESSION_SIZE) {
    logger.debug(
        "Dropping incoming Open Fronthaul message as its size is {} and it is smaller than the section header size.",
        deserializer.remaining_bytes());

    return false;
  }

  results.section_id = 0;
  results.section_id |= unsigned(deserializer.read<uint8_t>()) << 4;
  uint8_t section_and_others = deserializer.read<uint8_t>();
  results.section_id |= section_and_others >> 4;
  results.is_every_rb_used          = ((section_and_others >> 3) & 1U) == 0;
  results.use_current_symbol_number = ((section_and_others >> 2) & 1U) == 0;

  unsigned& start_prb = results.start_prb;
  start_prb           = 0;
  start_prb |= unsigned(section_and_others & 0x03) << 8;
  start_prb |= unsigned(deserializer.read<uint8_t>());

  unsigned& nof_prb = results.nof_prbs;
  nof_prb           = deserializer.read<uint8_t>();
  nof_prb           = (nof_prb == 0) ? ru_nof_prbs : nof_prb;

  return true;
}

bool uplane_message_decoder_static_compression_impl::decode_compression_header(
    uplane_section_params&             results,
    network_order_binary_deserializer& deserializer)
{
  switch (compression_params.type) {
    case compression_type::none:
    case compression_type::BFP:
    case compression_type::block_scaling:
    case compression_type::mu_law:
    case compression_type::modulation:
      return true;
    default:
      break;
  }

  if (deserializer.remaining_bytes() < sizeof(uint16_t)) {
    logger.debug("Dropping incoming Open Fronthaul message as its size is {} and it is smaller than the user data "
                 "compression length",
                 deserializer.remaining_bytes());

    return false;
  }

  results.ud_comp_len.emplace(deserializer.read<uint16_t>());

  return true;
}

/// \brief Decodes the compressed PRBs from the deserializer and returns true on success, otherwise false.
///
/// This function skips the udCompParam field.
///
/// \param[out] comp_prb Compressed PRBs to decode.
/// \param[in] deserializer Deserializer.
/// \param[in] prb_iq_data_size PRB size in bits.
/// \param[in] logger Logger.
/// \return True on success, false otherwise.
static bool decode_prbs_no_ud_comp_param_field(span<compressed_prb>               comp_prb,
                                               network_order_binary_deserializer& deserializer,
                                               units::bits                        prb_iq_data_size,
                                               srslog::basic_logger&              logger)
{
  if (deserializer.remaining_bytes() < prb_iq_data_size.round_up_to_bytes().value() * comp_prb.size()) {
    logger.debug("Dropping incoming Open Fronthaul message as its size is {} and it is smaller than the expected IQ "
                 "samples size {}",
                 deserializer.remaining_bytes(),
                 prb_iq_data_size.round_up_to_bytes().value() * comp_prb.size());

    return false;
  }
  // Read the samples from the deserializer.
  for (auto& prb : comp_prb) {
    // No need to read the udCompParam field.
    prb.set_compression_param(0);
    deserializer.read<uint8_t>(prb.get_buffer().first(prb_iq_data_size.round_up_to_bytes().value()));
  }

  return true;
}

/// \brief Decodes the compressed PRBs from the deserializer and returns true on success, otherwise false.
///
/// This function decodes the udCompParam field.
///
/// \param[out] comp_prb Compressed PRBs to decode.
/// \param[in] deserializer Deserializer.
/// \param[in] prb_iq_data_size PRB size in bits.
/// \param[in] logger Logger.
/// \return True on success, false otherwise.
static bool decode_prbs_with_ud_comp_param_field(span<compressed_prb>               comp_prb,
                                                 network_order_binary_deserializer& deserializer,
                                                 units::bits                        prb_iq_data_size,
                                                 srslog::basic_logger&              logger)
{
  // Add 1 byte to the PRB size as the udComParam must be decoded.
  units::bytes prb_bytes = prb_iq_data_size.round_up_to_bytes() + units::bytes(1);
  if (deserializer.remaining_bytes() < prb_bytes.value() * comp_prb.size()) {
    logger.debug("Dropping incoming Open Fronthaul message as its size is {} and it is smaller than the expected IQ "
                 "samples size {}",
                 deserializer.remaining_bytes(),
                 prb_bytes.value() * comp_prb.size());

    return false;
  }

  // For each PRB, udCompParam must be decoded.
  for (auto& prb : comp_prb) {
    prb.set_compression_param(deserializer.read<uint8_t>());
    deserializer.read<uint8_t>(prb.get_buffer().first(prb_iq_data_size.round_up_to_bytes().value()));
  }

  return true;
}

bool uplane_message_decoder_static_compression_impl::decode_iq_data(uplane_section_params&             results,
                                                                    network_order_binary_deserializer& deserializer)
{
  static_vector<compressed_prb, MAX_NOF_PRBS> comp_prbs(results.nof_prbs);
  units::bits prb_iq_data_size_bits(NOF_SUBCARRIERS_PER_RB * 2 * compression_params.data_width);

  //  udCompParam field is not present when compression type is none or modulation.
  bool is_iq_decoding_valid = false;
  if (compression_params.type == compression_type::none || compression_params.type == compression_type::modulation) {
    is_iq_decoding_valid = decode_prbs_no_ud_comp_param_field(comp_prbs, deserializer, prb_iq_data_size_bits, logger);
  } else {
    is_iq_decoding_valid = decode_prbs_with_ud_comp_param_field(comp_prbs, deserializer, prb_iq_data_size_bits, logger);
  }

  if (!is_iq_decoding_valid) {
    return false;
  }

  // Decompress the samples.
  results.iq_samples.resize(results.nof_prbs * NOF_SUBCARRIERS_PER_RB);
  decompressor.decompress(results.iq_samples, comp_prbs, compression_params);

  return true;
}
