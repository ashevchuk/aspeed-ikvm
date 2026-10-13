#pragma once

#include <freeipmi/freeipmi.h>

#include <string>

#include "ipmi_power.hpp"  // IpmiCredentials, IpmiError

// Shared out-of-band session setup for code that talks to libfreeipmi's raw
// (non-monitoring) API: chassis control and SDR cache reads.
namespace ipmi_raw {

// Matches ipmitool's default for "-I lanplus": HMAC-SHA1 auth, HMAC-SHA1-96
// integrity, AES-CBC-128 confidentiality.
constexpr uint8_t kCipherSuiteId = 3;

inline ipmi_ctx_t open_session(const IpmiCredentials &creds) {
  ipmi_ctx_t ctx = ipmi_ctx_create();
  if (!ctx) throw IpmiError("ipmi_ctx_create failed");

  if (ipmi_ctx_open_outofband_2_0(
          ctx, creds.host.c_str(), creds.username.c_str(), creds.password.c_str(),
          nullptr, 0, IPMI_PRIVILEGE_LEVEL_ADMIN, kCipherSuiteId, 0, 0,
          IPMI_WORKAROUND_FLAGS_DEFAULT, IPMI_FLAGS_DEFAULT) < 0) {
    std::string msg = ipmi_ctx_errormsg(ctx);
    ipmi_ctx_destroy(ctx);
    throw IpmiError("IPMI session to " + creds.host + " failed: " + msg);
  }
  return ctx;
}

inline void close_session(ipmi_ctx_t ctx) {
  ipmi_ctx_close(ctx);
  ipmi_ctx_destroy(ctx);
}

}  // namespace ipmi_raw
