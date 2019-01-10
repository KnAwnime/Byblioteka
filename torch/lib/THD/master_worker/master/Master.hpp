#pragma once

#include "../common/CommandChannel.hpp"
#include "../../base/DataChannel.hpp"

#include <memory>

namespace thd {
namespace master {

extern std::unique_ptr<MasterCommandChannel> masterCommandChannel;

} // namespace master
} // namespace thd
