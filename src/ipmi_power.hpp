#pragma once

#include <stdexcept>
#include <string>

struct IpmiCredentials {
  std::string host;
  std::string username;
  std::string password;
};

enum class PowerAction { On, Off, Cycle, HardReset, SoftShutdown };

struct PowerStatus {
  bool power_on = false;
};

class IpmiError : public std::runtime_error {
 public:
  explicit IpmiError(const std::string &what) : std::runtime_error(what) {}
};

PowerStatus ipmi_get_power_status(const IpmiCredentials &creds);
void ipmi_chassis_control(const IpmiCredentials &creds, PowerAction action);
