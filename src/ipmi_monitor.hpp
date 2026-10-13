#pragma once

#include <string>
#include <vector>

#include "ipmi_power.hpp"  // IpmiCredentials, IpmiError

struct SensorReading {
  std::string name;
  std::string category;  // "temperature" | "voltage" | "fan" | ... (see sensor_type_name)
  std::string units;
  std::string state;     // "nominal" | "warning" | "critical" | "unknown"
  bool has_value = false;
  double value = 0.0;

  // Thresholds from the sensor's SDR record, where defined by the BMC.
  // Lower/non-recoverable thresholds are omitted: rarely populated and not
  // useful for a simple gauge.
  bool has_lower_warning = false;
  double lower_warning = 0.0;
  bool has_lower_critical = false;
  double lower_critical = 0.0;
  bool has_upper_warning = false;
  double upper_warning = 0.0;
  bool has_upper_critical = false;
  double upper_critical = 0.0;
};

struct SelEntry {
  unsigned int record_id = 0;
  unsigned int timestamp = 0;  // seconds since epoch, 0 if not timestamped
  std::string sensor_type;
  std::string description;
};

std::vector<SensorReading> ipmi_read_sensors(const IpmiCredentials &creds);
std::vector<SelEntry> ipmi_read_sel(const IpmiCredentials &creds, unsigned int max_entries);
