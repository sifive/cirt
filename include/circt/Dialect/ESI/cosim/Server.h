//===- Server.h - ESI cosim RPC servers -------------------------*- C++ -*-===//
//
// Various classes used to implement the RPC server classes generated by
// CapnProto. Capnp C++ RPC servers are based on 'libkj' and its asyncrony
// model, which is very foreign.
//
//===----------------------------------------------------------------------===//

#ifndef CIRCT_DIALECT_ESI_COSIM_SERVER_H
#define CIRCT_DIALECT_ESI_COSIM_SERVER_H

#include "Endpoint.h"
#include "circt/Dialect/ESI/cosim/CosimDpi.capnp.h"
#include <capnp/any.h>
#include <capnp/ez-rpc.h>
#include <iostream>
#include <kj/async.h>
#include <map>
#include <thread>

namespace circt {
namespace esi {
namespace cosim {

/// Implements the `EsiDpiEndpoint` interface from the RPC schema. Mostly a
/// wrapper around an `EndPoint` object. Whereas the `EndPoints` are long-lived
/// (associate with the RTL endpoint), this class is constructed/destructed when
/// the client open()s it.
class EndpointServer
    : public EsiDpiEndpoint<capnp::AnyPointer, capnp::AnyPointer>::Server {
  /// The wrapped endpoint.
  Endpoint *endpoint;
  /// Signals that this endpoint has been opened by a client and hasn't been
  /// closed by said client.
  bool open;

public:
  EndpointServer(Endpoint *ep) : endpoint(ep), open(true) {}

  virtual ~EndpointServer() {
    if (open)
      endpoint->returnForUse();
  }

  Endpoint *getEndPoint() { return endpoint; }

  kj::Promise<void> send(SendContext);
  kj::Promise<void> recv(RecvContext);
  kj::Promise<void> close(CloseContext);
};

/// Implements the `CosimDpiServer` interface from the RPC schema.
class CosimServer : public CosimDpiServer::Server {
  /// The registry of endpoints.
  EndpointRegistry *reg;

public:
  CosimServer(EndpointRegistry *reg) : reg(reg) {}
  virtual ~CosimServer() {}

  /// List all the registered interfaces.
  kj::Promise<void> list(ListContext ctxt);
  /// Open a specific interface, locking it in the process.
  kj::Promise<void> open(OpenContext ctxt);
};

/// The main RpcServer. Does not implement any capnp RPC interfaces but contains
/// the capnp main RPC server. We run the capnp server in its own thread to be
/// more responsive to network traffic and so we do not have to reason about
/// interactions between the capnp (really libkj) async model.
class RpcServer {
public:
  EndpointRegistry endpoints;

  RpcServer() : rpcServer(nullptr), mainThread(nullptr), stopSig(false) {}
  ~RpcServer();

  void run(uint16_t port);
  void stop();

private:
  void mainLoop(uint16_t port);

  capnp::EzRpcServer *rpcServer;
  std::thread *mainThread;
  volatile bool stopSig;
};

} // namespace cosim
} // namespace esi
} // namespace circt

#endif
