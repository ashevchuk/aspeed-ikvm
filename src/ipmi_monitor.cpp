#include "ipmi_monitor.hpp"

#include <sys/stat.h>

#include <freeipmi/freeipmi.h>
#include <ipmi_monitoring.h>
#include <ipmi_monitoring_bitmasks.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <map>

#include "ipmi_raw_session.hpp"

namespace {

constexpr int kCipherSuiteId = 3;  // matches ipmitool's "-I lanplus" default
constexpr const char *kSdrCacheDir = "data/sdr-cache";

void ensure_sdr_cache_dir() { mkdir(kSdrCacheDir, S_IRWXU); }

ipmi_monitoring_ipmi_config build_config(const IpmiCredentials &creds) {
  ipmi_monitoring_ipmi_config config{};
  config.protocol_version = IPMI_MONITORING_PROTOCOL_VERSION_2_0;
  config.username = const_cast<char *>(creds.username.c_str());
  config.password = const_cast<char *>(creds.password.c_str());
  config.k_g = nullptr;
  config.k_g_len = 0;
  config.privilege_level = IPMI_MONITORING_PRIVILEGE_LEVEL_ADMIN;
  config.authentication_type = IPMI_MONITORING_AUTHENTICATION_TYPE_MD5;
  config.cipher_suite_id = kCipherSuiteId;
  config.session_timeout_len = 0;
  config.retransmission_timeout_len = 0;
  config.workaround_flags = 0;
  return config;
}

ipmi_monitoring_ctx_t create_ctx() {
  static bool initialized = false;
  if (!initialized) {
    int errnum = 0;
    if (ipmi_monitoring_init(0, &errnum) < 0)
      throw IpmiError(std::string("ipmi_monitoring_init failed: errnum=") + std::to_string(errnum));
    initialized = true;
  }

  ipmi_monitoring_ctx_t ctx = ipmi_monitoring_ctx_create();
  if (!ctx) throw IpmiError("ipmi_monitoring_ctx_create failed");

  ensure_sdr_cache_dir();
  ipmi_monitoring_ctx_sdr_cache_directory(ctx, kSdrCacheDir);
  return ctx;
}

std::string sensor_type_name(int type) {
  switch (type) {
    case IPMI_MONITORING_SENSOR_TYPE_TEMPERATURE:
      return "temperature";
    case IPMI_MONITORING_SENSOR_TYPE_VOLTAGE:
      return "voltage";
    case IPMI_MONITORING_SENSOR_TYPE_CURRENT:
      return "current";
    case IPMI_MONITORING_SENSOR_TYPE_FAN:
      return "fan";
    case IPMI_MONITORING_SENSOR_TYPE_POWER_SUPPLY:
      return "power_supply";
    case IPMI_MONITORING_SENSOR_TYPE_POWER_UNIT:
      return "power_unit";
    case IPMI_MONITORING_SENSOR_TYPE_PROCESSOR:
      return "processor";
    case IPMI_MONITORING_SENSOR_TYPE_MEMORY:
      return "memory";
    case IPMI_MONITORING_SENSOR_TYPE_DRIVE_SLOT:
      return "drive_slot";
    case IPMI_MONITORING_SENSOR_TYPE_SYSTEM_EVENT:
      return "system_event";
    case IPMI_MONITORING_SENSOR_TYPE_WATCHDOG1:
    case IPMI_MONITORING_SENSOR_TYPE_WATCHDOG2:
      return "watchdog";
    default:
      return "other";
  }
}

std::string sensor_units_name(int units) {
  switch (units) {
    case IPMI_MONITORING_SENSOR_UNITS_CELSIUS:
      return "C";
    case IPMI_MONITORING_SENSOR_UNITS_FAHRENHEIT:
      return "F";
    case IPMI_MONITORING_SENSOR_UNITS_VOLTS:
      return "V";
    case IPMI_MONITORING_SENSOR_UNITS_AMPS:
      return "A";
    case IPMI_MONITORING_SENSOR_UNITS_RPM:
      return "RPM";
    case IPMI_MONITORING_SENSOR_UNITS_WATTS:
      return "W";
    case IPMI_MONITORING_SENSOR_UNITS_PERCENT:
      return "%";
    default:
      return "";
  }
}

std::string sensor_state_name(int state) {
  switch (state) {
    case IPMI_MONITORING_STATE_NOMINAL:
      return "nominal";
    case IPMI_MONITORING_STATE_WARNING:
      return "warning";
    case IPMI_MONITORING_STATE_CRITICAL:
      return "critical";
    default:
      return "unknown";
  }
}

struct SdrThresholds {
  bool has_lower_warning = false;
  double lower_warning = 0.0;
  bool has_lower_critical = false;
  double lower_critical = 0.0;
  bool has_upper_warning = false;
  double upper_warning = 0.0;
  bool has_upper_critical = false;
  double upper_critical = 0.0;
};

std::string sdr_cache_path(const std::string &host) {
  return std::string(kSdrCacheDir) + "/" + host + ".sdr";
}

// Thresholds live in each sensor's Full Sensor Record in the SDR repository,
// not in anything libipmimonitoring exposes - that library only reports the
// derived nominal/warning/critical state. Cached to disk like ipmitool does,
// since the SDR repository rarely changes and re-downloading it on every
// sensor read would mean a second full BMC round trip each time.
std::map<uint16_t, SdrThresholds> read_sdr_thresholds(const IpmiCredentials &creds) {
  std::map<uint16_t, SdrThresholds> result;

  ipmi_sdr_ctx_t sdr_ctx = ipmi_sdr_ctx_create();
  if (!sdr_ctx) throw IpmiError("ipmi_sdr_ctx_create failed");

  ensure_sdr_cache_dir();
  std::string cache_path = sdr_cache_path(creds.host);

  struct stat st;
  if (stat(cache_path.c_str(), &st) != 0) {
    ipmi_ctx_t raw_ctx = ipmi_raw::open_session(creds);
    int rv = ipmi_sdr_cache_create(sdr_ctx, raw_ctx, cache_path.c_str(),
                                    IPMI_SDR_CACHE_CREATE_FLAGS_DEFAULT, nullptr, nullptr);
    ipmi_raw::close_session(raw_ctx);
    if (rv < 0) {
      std::string msg = ipmi_sdr_ctx_errormsg(sdr_ctx);
      ipmi_sdr_ctx_destroy(sdr_ctx);
      throw IpmiError("SDR cache create for " + creds.host + " failed: " + msg);
    }
  }

  if (ipmi_sdr_cache_open(sdr_ctx, nullptr, cache_path.c_str()) < 0) {
    std::string msg = ipmi_sdr_ctx_errormsg(sdr_ctx);
    ipmi_sdr_ctx_destroy(sdr_ctx);
    throw IpmiError("SDR cache open for " + creds.host + " failed: " + msg);
  }

  uint16_t record_count = 0;
  ipmi_sdr_cache_record_count(sdr_ctx, &record_count);

  uint8_t record_buf[IPMI_SDR_MAX_RECORD_LENGTH];

  if (record_count > 0 && ipmi_sdr_cache_first(sdr_ctx) == 0) {
    for (uint16_t i = 0; i < record_count; ++i) {
      int len = ipmi_sdr_cache_record_read(sdr_ctx, record_buf, sizeof(record_buf));
      if (len > 0) {
        uint16_t record_id = 0;
        uint8_t record_type = 0;
        if (ipmi_sdr_parse_record_id_and_type(sdr_ctx, record_buf, static_cast<unsigned int>(len),
                                               &record_id, &record_type) == 0 &&
            record_type == IPMI_SDR_FORMAT_FULL_SENSOR_RECORD) {
          // The SDR record always has storage for all six threshold bytes;
          // only the ones flagged readable here hold a BMC-configured value
          // - the rest are leftover bytes that decode to nonsense (ipmitool
          // shows those as "na" rather than a number).
          uint8_t readable_lower_nc = 0, readable_lower_c = 0, readable_lower_nr = 0;
          uint8_t readable_upper_nc = 0, readable_upper_c = 0, readable_upper_nr = 0;
          int readable_rv = ipmi_sdr_parse_threshold_readable(
              sdr_ctx, record_buf, static_cast<unsigned int>(len), &readable_lower_nc, &readable_lower_c,
              &readable_lower_nr, &readable_upper_nc, &readable_upper_c, &readable_upper_nr);

          double *lower_nc = nullptr, *lower_c = nullptr, *lower_nr = nullptr;
          double *upper_nc = nullptr, *upper_c = nullptr, *upper_nr = nullptr;
          if (readable_rv == 0 &&
              ipmi_sdr_parse_thresholds(sdr_ctx, record_buf, static_cast<unsigned int>(len), &lower_nc,
                                         &lower_c, &lower_nr, &upper_nc, &upper_c, &upper_nr) == 0) {
            SdrThresholds t;
            if (lower_nc && readable_lower_nc) {
              t.has_lower_warning = true;
              t.lower_warning = *lower_nc;
            }
            if (lower_c && readable_lower_c) {
              t.has_lower_critical = true;
              t.lower_critical = *lower_c;
            }
            if (upper_nc && readable_upper_nc) {
              t.has_upper_warning = true;
              t.upper_warning = *upper_nc;
            }
            if (upper_c && readable_upper_c) {
              t.has_upper_critical = true;
              t.upper_critical = *upper_c;
            }
            free(lower_nc);
            free(lower_c);
            free(lower_nr);
            free(upper_nc);
            free(upper_c);
            free(upper_nr);
            result[record_id] = t;
          }
        }
      }
      if (i + 1 < record_count) ipmi_sdr_cache_next(sdr_ctx);
    }
  }

  ipmi_sdr_cache_close(sdr_ctx);
  ipmi_sdr_ctx_destroy(sdr_ctx);
  return result;
}

}  // namespace

std::vector<SensorReading> ipmi_read_sensors(const IpmiCredentials &creds) {
  ipmi_monitoring_ctx_t ctx = create_ctx();
  ipmi_monitoring_ipmi_config config = build_config(creds);

  unsigned int flags = IPMI_MONITORING_SENSOR_READING_FLAGS_IGNORE_NON_INTERPRETABLE_SENSORS;

  int count = ipmi_monitoring_sensor_readings_by_record_id(ctx, creds.host.c_str(), &config, flags,
                                                            nullptr, 0, nullptr, nullptr);
  if (count < 0) {
    std::string msg = ipmi_monitoring_ctx_errormsg(ctx);
    ipmi_monitoring_ctx_destroy(ctx);
    throw IpmiError("sensor read on " + creds.host + " failed: " + msg);
  }

  std::vector<SensorReading> readings;
  std::vector<uint16_t> record_ids;
  readings.reserve(static_cast<size_t>(count));
  record_ids.reserve(static_cast<size_t>(count));

  if (ipmi_monitoring_sensor_iterator_first(ctx) == 0) {
    for (int i = 0; i < count; ++i) {
      SensorReading reading;
      record_ids.push_back(static_cast<uint16_t>(ipmi_monitoring_sensor_read_record_id(ctx)));

      reading.category = sensor_type_name(ipmi_monitoring_sensor_read_sensor_type(ctx));
      const char *name = ipmi_monitoring_sensor_read_sensor_name(ctx);
      reading.name = name ? name : reading.category;
      reading.units = sensor_units_name(ipmi_monitoring_sensor_read_sensor_units(ctx));
      reading.state = sensor_state_name(ipmi_monitoring_sensor_read_sensor_state(ctx));

      int reading_type = ipmi_monitoring_sensor_read_sensor_reading_type(ctx);
      void *value_ptr = ipmi_monitoring_sensor_read_sensor_reading(ctx);
      if (value_ptr) {
        reading.has_value = true;
        switch (reading_type) {
          case IPMI_MONITORING_SENSOR_READING_TYPE_UNSIGNED_INTEGER8_BOOL:
            reading.value = *static_cast<uint8_t *>(value_ptr);
            break;
          case IPMI_MONITORING_SENSOR_READING_TYPE_UNSIGNED_INTEGER32:
            reading.value = *static_cast<uint32_t *>(value_ptr);
            break;
          case IPMI_MONITORING_SENSOR_READING_TYPE_DOUBLE:
            reading.value = *static_cast<double *>(value_ptr);
            break;
          default:
            reading.has_value = false;
            break;
        }
      }

      readings.push_back(std::move(reading));

      if (i + 1 < count) ipmi_monitoring_sensor_iterator_next(ctx);
    }
  }

  ipmi_monitoring_ctx_destroy(ctx);

  try {
    std::map<uint16_t, SdrThresholds> thresholds = read_sdr_thresholds(creds);
    for (size_t i = 0; i < readings.size(); ++i) {
      auto it = thresholds.find(record_ids[i]);
      if (it == thresholds.end()) continue;
      const SdrThresholds &t = it->second;
      readings[i].has_lower_warning = t.has_lower_warning;
      readings[i].lower_warning = t.lower_warning;
      readings[i].has_lower_critical = t.has_lower_critical;
      readings[i].lower_critical = t.lower_critical;
      readings[i].has_upper_warning = t.has_upper_warning;
      readings[i].upper_warning = t.upper_warning;
      readings[i].has_upper_critical = t.has_upper_critical;
      readings[i].upper_critical = t.upper_critical;
    }
  } catch (const IpmiError &) {
    // Thresholds are a nice-to-have on top of the values/state we already
    // have; don't fail the whole sensor read if the SDR fetch fails.
  }

  return readings;
}

std::vector<SelEntry> ipmi_read_sel(const IpmiCredentials &creds, unsigned int max_entries) {
  ipmi_monitoring_ctx_t ctx = create_ctx();
  ipmi_monitoring_ipmi_config config = build_config(creds);

  int count = ipmi_monitoring_sel_by_record_id(ctx, creds.host.c_str(), &config, 0, nullptr, 0,
                                                nullptr, nullptr);
  if (count < 0) {
    std::string msg = ipmi_monitoring_ctx_errormsg(ctx);
    ipmi_monitoring_ctx_destroy(ctx);
    throw IpmiError("SEL read on " + creds.host + " failed: " + msg);
  }

  std::vector<SelEntry> entries;
  unsigned int limit = max_entries == 0 ? static_cast<unsigned int>(count)
                                         : std::min(max_entries, static_cast<unsigned int>(count));
  entries.reserve(limit);

  if (ipmi_monitoring_sel_iterator_first(ctx) == 0) {
    for (int i = 0; i < count && entries.size() < limit; ++i) {
      SelEntry entry;
      entry.record_id = static_cast<unsigned int>(ipmi_monitoring_sel_read_record_id(ctx));

      unsigned int ts = 0;
      if (ipmi_monitoring_sel_read_timestamp(ctx, &ts) == 0) entry.timestamp = ts;

      int record_type_class = ipmi_monitoring_sel_read_record_type_class(ctx);
      if (record_type_class == IPMI_MONITORING_SEL_RECORD_TYPE_CLASS_SYSTEM_EVENT_RECORD) {
        entry.sensor_type = sensor_type_name(ipmi_monitoring_sel_read_sensor_type(ctx));

        const char *sensor_name = ipmi_monitoring_sel_read_sensor_name(ctx);
        const char *offset_string = ipmi_monitoring_sel_read_event_offset_string(ctx);
        entry.description = (sensor_name ? std::string(sensor_name) : entry.sensor_type) + ": " +
                             (offset_string ? offset_string : "");
      } else {
        entry.sensor_type = "oem";
        entry.description = "OEM / non-standard SEL record";
      }

      entries.push_back(std::move(entry));

      if (i + 1 < count) ipmi_monitoring_sel_iterator_next(ctx);
    }
  }

  ipmi_monitoring_ctx_destroy(ctx);
  return entries;
}
