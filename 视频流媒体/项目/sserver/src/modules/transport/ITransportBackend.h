#ifndef SSERVER_MODULES_TRANSPORT_ITRANSPORTBACKEND_H
#define SSERVER_MODULES_TRANSPORT_ITRANSPORTBACKEND_H

#include <memory>
#include <string>

#include "common/model/EncodedFrame.h"
#include "core/ApplicationContext.h"
#include "core/ModuleState.h"
#include "modules/transport/Transport.h"

namespace sserver {
namespace modules {
namespace transport {

class ITransportBackend {
public:
    virtual ~ITransportBackend() = default;

    virtual bool initialize(const core::ApplicationContext &context) = 0;
    virtual bool start() = 0;
    virtual void stop() = 0;
    virtual void shutdown() = 0;
    virtual core::ModuleState state() const = 0;
    virtual void Broadcast(common::model::EncodedFramePtr frame) = 0;
    virtual int bound_port() const = 0;
    virtual TransportBackend backend() const = 0;
    virtual const std::string &backend_name() const = 0;
};

}  // namespace transport
}  // namespace modules
}  // namespace sserver

#endif  // SSERVER_MODULES_TRANSPORT_ITRANSPORTBACKEND_H
