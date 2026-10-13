#include "ipmi_power.hpp"

#include <freeipmi/freeipmi.h>

#include "ipmi_raw_session.hpp"

using ipmi_raw::close_session;
using ipmi_raw::open_session;

namespace {

uint8_t chassis_control_value(PowerAction action) {
  switch (action) {
    case PowerAction::Off:
      return IPMI_CHASSIS_CONTROL_POWER_DOWN;
    case PowerAction::On:
      return IPMI_CHASSIS_CONTROL_POWER_UP;
    case PowerAction::Cycle:
      return IPMI_CHASSIS_CONTROL_POWER_CYCLE;
    case PowerAction::HardReset:
      return IPMI_CHASSIS_CONTROL_HARD_RESET;
    case PowerAction::SoftShutdown:
      return IPMI_CHASSIS_CONTROL_INITIATE_SOFT_SHUTDOWN;
  }
  throw IpmiError("unknown power action");
}

}  // namespace

PowerStatus ipmi_get_power_status(const IpmiCredentials &creds) {
  ipmi_ctx_t ctx = open_session(creds);

  fiid_obj_t obj = fiid_obj_create(tmpl_cmd_get_chassis_status_rs);
  if (!obj) {
    close_session(ctx);
    throw IpmiError("fiid_obj_create failed");
  }

  PowerStatus status;
  if (ipmi_cmd_get_chassis_status(ctx, obj) < 0) {
    std::string msg = ipmi_ctx_errormsg(ctx);
    fiid_obj_destroy(obj);
    close_session(ctx);
    throw IpmiError("get chassis status on " + creds.host + " failed: " + msg);
  }

  uint64_t power_is_on = 0;
  fiid_obj_get(obj, "current_power_state.power_is_on", &power_is_on);
  status.power_on = power_is_on != 0;

  fiid_obj_destroy(obj);
  close_session(ctx);
  return status;
}

void ipmi_chassis_control(const IpmiCredentials &creds, PowerAction action) {
  ipmi_ctx_t ctx = open_session(creds);

  fiid_obj_t obj = fiid_obj_create(tmpl_cmd_chassis_control_rs);
  if (!obj) {
    close_session(ctx);
    throw IpmiError("fiid_obj_create failed");
  }

  int rv = ipmi_cmd_chassis_control(ctx, chassis_control_value(action), obj);
  if (rv < 0) {
    std::string msg = ipmi_ctx_errormsg(ctx);
    fiid_obj_destroy(obj);
    close_session(ctx);
    throw IpmiError("chassis control on " + creds.host + " failed: " + msg);
  }

  fiid_obj_destroy(obj);
  close_session(ctx);
}
