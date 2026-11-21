#pragma once

#include "scale_protocol.h"

namespace scale {

bool beginReceiver();
const char *receiverStatus();
Profile getProfile();
bool saveProfile(const Profile &profile);

}  // namespace scale
