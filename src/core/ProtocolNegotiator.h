#pragma once

#include "core/Types.h"

#include <QString>
#include <atomic>
#include <vector>

namespace xsync {

class SshClient;

struct NegotiationPlan {
    TransferProtocol primary = TransferProtocol::Sftp;
    std::vector<TransferProtocol> fallbacks;
    QString reason;
};

class ProtocolNegotiator {
public:
    // Probes local OS capabilities (rsync, ssh, scp, flavor). Cached after first run.
    static EndpointCapabilities probeLocal(bool forceRefresh = false);

    // Probes remote host capabilities over an already-connected SshClient.
    static EndpointCapabilities probeRemote(SshClient& client,
                                            const std::atomic<bool>* cancelFlag = nullptr);

    // Pure negotiation decision engine: selects primary protocol + ordered fallbacks.
    // Core rule: Prefer rsync if BOTH sides have it; otherwise degrade cleanly to SFTP -> SCP.
    // For Remote <-> Remote: respects RemoteToRemotePolicy (DirectThenRelay / RelayOnly / DirectOnly).
    static NegotiationPlan negotiate(const EndpointSpec& src,
                                     const EndpointSpec& dst,
                                     const EndpointCapabilities& srcCaps,
                                     const EndpointCapabilities& dstCaps,
                                     ProtocolPreference pref,
                                     RemoteToRemotePolicy r2rPolicy);

    // Finds local OpenSSH executable path (used by rsync -e "ssh ...")
    static QString findLocalSshExecutable();
};

}  // namespace xsync
