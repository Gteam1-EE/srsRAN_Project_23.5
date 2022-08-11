/*
 *
 * Copyright 2013-2022 Software Radio Systems Limited
 *
 * By using this file, you agree to the terms and conditions set
 * forth in the LICENSE file which can be found at the top level of
 * the distribution.
 *
 */

#pragma once

#include "srsgnb/ran/du_types.h"
#include "srsgnb/ran/lcid.h"
#include "srsgnb/srslog/srslog.h"

namespace fmt {

template <size_t N>
const char* to_c_str(fmt::basic_memory_buffer<char, N>& mem_buffer)
{
  mem_buffer.push_back('\0');
  return mem_buffer.data();
}

} // namespace fmt

namespace srsgnb {

/// Class used to store common logging parameters for all types RLC entities.
/// It provides logging helpers, so that the UE index and LCID are always logged.
///
/// \param log_name name of the logger we want to use (e.g. RLC, PDCP, etc.)
/// \param du_index UE identifier within the DU
/// \param lcid LCID for the bearer
class bearer_logger
{
public:
  bearer_logger(const std::string& log_name, du_ue_index_t du_index, lcid_t lcid) :
    du_index(du_index), lcid(lcid), logger(srslog::fetch_basic_logger(log_name, false))
  {
  }

  template <typename... Args>
  void log_debug(const char* fmt, Args&&... args)
  {
    log_helper(logger.debug, fmt, std::forward<Args>(args)...);
  }
  template <typename... Args>
  void log_info(const char* fmt, Args&&... args)
  {
    log_helper(logger.info, fmt, std::forward<Args>(args)...);
  }
  template <typename... Args>
  void log_warning(const char* fmt, Args&&... args)
  {
    log_helper(logger.warning, fmt, std::forward<Args>(args)...);
  }
  template <typename... Args>
  void log_error(const char* fmt, Args&&... args)
  {
    log_helper(logger.error, fmt, std::forward<Args>(args)...);
  }

  template <typename... Args>
  void log(const srslog::basic_levels level, const char* fmt, Args&&... args)
  {
    switch (level) {
      case srslog::basic_levels::debug:
        log_debug(fmt, std::forward<Args>(args)...);
        break;
      case srslog::basic_levels::info:
        log_info(fmt, std::forward<Args>(args)...);
        break;
      case srslog::basic_levels::warning:
        log_warning(fmt, std::forward<Args>(args)...);
        break;
      case srslog::basic_levels::error:
        log_error(fmt, std::forward<Args>(args)...);
        break;
      case srslog::basic_levels::none:
        // skip
        break;
      default:
        log_warning("Unsupported log level: {}", basic_level_to_string(level));
        break;
    }
  }

  template <typename It, typename... Args>
  void log_debug(It it_begin, It it_end, const char* fmt, Args&&... args)
  {
    log_helper(it_begin, it_end, logger.debug, fmt, std::forward<Args>(args)...);
  }
  template <typename It, typename... Args>
  void log_info(It it_begin, It it_end, const char* fmt, Args&&... args)
  {
    log_helper(it_begin, it_end, logger.info, fmt, std::forward<Args>(args)...);
  }
  template <typename It, typename... Args>
  void log_warning(It it_begin, It it_end, const char* fmt, Args&&... args)
  {
    log_helper(it_begin, it_end, logger.warning, fmt, std::forward<Args>(args)...);
  }
  template <typename It, typename... Args>
  void log_error(It it_begin, It it_end, const char* fmt, Args&&... args)
  {
    log_helper(it_begin, it_end, logger.error, fmt, std::forward<Args>(args)...);
  }

  const du_ue_index_t du_index;
  const lcid_t        lcid;

private:
  srslog::basic_logger& logger;

  template <typename... Args>
  void log_helper(srslog::log_channel& channel, const char* fmt, Args&&... args)
  {
    if (!channel.enabled()) {
      return;
    }
    fmt::memory_buffer buffer;
    fmt::format_to(buffer, fmt, std::forward<Args>(args)...);
    channel("UE={}, LCID={}: {}", du_index, lcid, fmt::to_c_str(buffer));
  }

  template <typename It, typename... Args>
  void log_helper(It it_begin, It it_end, srslog::log_channel& channel, const char* fmt, Args&&... args)
  {
    if (!channel.enabled()) {
      return;
    }
    fmt::memory_buffer buffer;
    fmt::format_to(buffer, fmt, std::forward<Args>(args)...);
    channel(it_begin, it_end, "UE={}, LCID={}: {}", du_index, lcid, fmt::to_c_str(buffer));
  }
};

} // namespace srsgnb
