#ifndef SSERVER_CORE_APPLICATIONCONTEXT_H
#define SSERVER_CORE_APPLICATIONCONTEXT_H

#include "config/AppConfig.h"

namespace sserver {
namespace core {

struct ApplicationContext {
    config::AppConfig config;
};

}  // namespace core
}  // namespace sserver

#endif  // SSERVER_CORE_APPLICATIONCONTEXT_H
