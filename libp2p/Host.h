/*
        This file is part of cpp-ethereum.

        cpp-ethereum is free software: you can redistribute it and/or modify
        it under the terms of the GNU General Public License as published by
        the Free Software Foundation, either version 3 of the License, or
        (at your option) any later version.

        cpp-ethereum is distributed in the hope that it will be useful,
        but WITHOUT ANY WARRANTY; without even the implied warranty of
        MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
        GNU General Public License for more details.

        You should have received a copy of the GNU General Public License
        along with cpp-ethereum.  If not, see <http://www.gnu.org/licenses/>.
*/
/** @file Host.h
 * @author Alex Leverington <nessence@gmail.com>
 * @author Gav Wood <i@gavwood.com>
 * @date 2014
 * @author toxotguo
 * @date 2018
 */
#pragma once
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <utility>
#include <vector>

#include "Common.h"
#include "HostCapabilityFace.h"
#include "Network.h"
#include "Peer.h"
#include "RLPXSocket.h"
#include <libdevcore/AsioInterface.h>
#include <libdevcore/Guards.h>
#include <libdevcore/Worker.h>
#include <libdevcrypto/Common.h>
#include <libdevcrypto/ECDHE.h>

namespace ba = boost::asio;
namespace bi = ba::ip;

namespace dev
{
namespace p2p
{
class RLPXHandshake;
class Host : public Worker
{
public:
    /// constructor function : init Host with specified client version,
    /// keypair and network config
    Host(std::string const& _clientVersion, KeyPair const& _alias, NetworkConfig const& _n,
        shared_ptr<AsioInterface>& m_asioInterface);
    ~Host();

    /// ------get interfaces ------
    /// get client version
    std::string const& clientVersion() const { return m_clientVersion; }
    /// get network configs
    NetworkConfig const& networkConfig() const { return m_netConfigs; }
    /// get io_service object  pointer
    ba::io_service* ioService() { return &m_ioService; }
    /// get keypair
    KeyPair const& keyPair() const { return m_alias; }
    /// get node id(public key)
    NodeID const& id() const { return m_alias.pub(); }
    /// get private key of the node
    Secret sec() const { return m_alias.secret(); }
    /// post and dispatch handlers
    boost::asio::io_service::strand* strand() { return &m_strand; }
    /// get the listen port
    uint16_t const& listenPort() const { return m_listenPort; }
    /// get the public address endpoint
    /// (obtained from listenAddress or network interface address by callback determinePublic)
    bi::tcp::endpoint const& tcpPublic() const { return m_tcpPublic; }
    /// get the number of connected peers
    size_t peerCount() const;
    /// get session map
    std::unordered_map<NodeID, std::weak_ptr<SessionFace>>& sessions() { return m_sessions; }
    /// get mutex of sessions
    RecursiveMutex& mutexSessions() { return x_sessions; }
    /// get m_staticNodes
    std::map<NodeIPEndpoint, NodeID>* staticNodes() { return &m_staticNodes; }
    /// get the peer slots
    unsigned peerSlots(PeerSlotType _type) const
    {
        /// to remove warning(maybe _type will be used in the future)
        (void)_type;
        return m_maxPeerCount;
    }
    /// to avoid too many connected sessions
    bool peerSlotsAvailable(PeerSlotType _type = Ingress)
    {
        Guard l(x_pendingNodeConns);
        return peerCount() + m_pendingPeerConns.size() < peerSlots(_type);
    }
    /// judge whether the specified node has been connected
    bool isConnected(const NodeID& nodeID)
    {
        auto session = peerSession(nodeID);
        if (session && session->isConnected())
            return true;
        return false;
    }
    /// get status of worker thread
    bool isStarted() const { return isWorking(); }
    /// get peers(maybe called by EthereumHost)
    Peers getPeers() const
    {
        Peers ret;
        try
        {
            RecursiveGuard l(x_sessions);
            for (auto const& i : m_peers)
                ret.push_back(*i.second);
        }
        catch (...)
        {
        }
        return ret;
    }

    /// ------set interfaces ------
    /// set m_reconnectnow to be true, reconnect all peers when callback keepAlivePeers
    void reconnectNow()
    {
        Guard l(x_reconnectnow);
        m_reconnectnow = true;
    }

    /// set static nodes
    void setStaticNodes(const std::map<NodeIPEndpoint, NodeID>& staticNodes)
    {
        m_staticNodes = staticNodes;
    }

    ///------ Network and worker threads related ------
    /// the working entry of libp2p(called by when init FISCO-BCOS to start the p2p network)
    void start();
    /// called by start() to start the network
    virtual void startedWorking();
    /// start peer sessions after handshake succeed(called by RLPxHandshake), mainly include four
    /// functions:
    /// 1. disconnect connecting host with invalid capability
    /// 2. modify m_peers && disconnect already-connected session
    /// 3. modify m_sessions and m_staticNodes
    /// 4. start new session (session->start())
    void startPeerSession(
        Public const& _pub, RLP const& _rlp, std::shared_ptr<RLPXSocket> const& _s);
    /// remove invalid RLPXHandshake object from m_connecting
    /// remove expired timer
    /// modify alived peers to m_peers
    /// reconnect all nodes recorded in m_staticNodes periodically
    void run(boost::system::error_code const& error);
    /// start the network by callback io_serivce.run
    void doWork();
    /// stop the network and worker thread
    void stop();
    /// clean resources (include both network, socket resources) when stop working
    void doneWorking();

    /// get the network status
    /// @return true : the network has been started
    /// @return false : the network has been stopped
    bool haveNetwork() const
    {
        Guard l(x_runTimer);
        return m_run;
    }

    /// @return: whether specified capabilities exists
    virtual bool haveCapability(CapDesc const& _name) const
    {
        return m_capabilities.count(_name) != 0;
    }

    /// register capability
    template <class T>
    std::shared_ptr<T> registerCapability(std::shared_ptr<T> const& _t)
    {
        _t->m_host = this;
        m_capabilities[std::make_pair(T::staticName(), T::staticVersion())] = _t;
        return _t;
    }
    /// add capability
    template <class T>
    void addCapability(std::shared_ptr<T> const& _p, std::string const& _name, u256 const& _version)
    {
        m_capabilities[std::make_pair(_name, _version)] = _p;
    }
    /// get all cap descriptions
    virtual CapDescs caps() const
    {
        CapDescs ret;
        for (auto const& i : m_capabilities)
            ret.push_back(i.first);
        return ret;
    }

    template <class T>
    std::shared_ptr<T> cap() const
    {
        try
        {
            return std::static_pointer_cast<T>(
                m_capabilities.at(std::make_pair(T::staticName(), T::staticVersion())));
        }
        catch (...)
        {
            return nullptr;
        }
    }

    /// Get session by id
    std::shared_ptr<SessionFace> peerSession(NodeID const& _id)
    {
        RecursiveGuard l(x_sessions);
        return m_sessions.count(_id) ? m_sessions[_id].lock() : nullptr;
    }
    bytes saveNetwork() const;

    /// TODO: implement 'disconnectByNodeId'
    void disconnectByNodeId(const std::string& sNodeId) {}

protected:  /// protected functions
    /// called by 'startedWorking' to accept connections
    virtual void runAcceptor();
    /// functions called after openssl handshake,
    /// maily to get node id and verify whether the certificate has been expired
    /// @return: node id of the connected peer
    std::function<bool(bool, boost::asio::ssl::verify_context&)> newVerifyCallback(
        std::shared_ptr<std::string> nodeIDOut);
    /// @return true: the given certificate has been expired
    /// @return false: the given certificate has not been expired
    static bool isExpired(ba::ssl::verify_context& ctx);
    /// server calls handshakeServer to after handshake, mainly calls RLPxHandshake to obtain
    /// informations(client version, caps, etc),start peer session and start accepting procedure
    /// repeatedly
    void handshakeServer(const boost::system::error_code& error,
        std::shared_ptr<std::string>& endpointPublicKey, std::shared_ptr<RLPXSocket> socket);

    /// update m_sessions and m_peers periodically
    /// drop the unconnected/invalid sessions periodically
    void keepAlivePeers();
    /// @return true: the specified peer node has alived-sessions
    /// @ return false: the specified peer node has no-alived-sessions
    bool havePeerSession(NodeID const& _id)
    {
        RecursiveGuard l(x_sessions);
        if (m_sessions.count(_id) && m_sessions[_id].lock())
            return true;
        return false;
    }
    /// reconnect to all unconnected peers recorded in m_staticNodes
    void reconnectAllNodes();
    /// connect to the server
    void connect(NodeIPEndpoint const& _nodeIPEndpoint);
    /// start RLPxHandshake procedure after ssl handshake succeed
    void handshakeClient(const boost::system::error_code& error, std::shared_ptr<RLPXSocket> socket,
        std::shared_ptr<std::string>& endpointPublicKey, NodeIPEndpoint& _nodeIPEndpoint);
    inline void determinePublic() { m_tcpPublic = Network::determinePublic(m_netConfigs); }

protected:  /// protected members(for unit testing)
    /// values inited by contructor
    /// Our version string
    std::string m_clientVersion;
    /// Network settings.
    NetworkConfig m_netConfigs;
    /// Interface addresses.
    std::set<bi::address> m_ifAddresses;
    /// I/O handler
    ba::io_service m_ioService;
    /// Listening acceptor.
    bi::tcp::acceptor m_tcp4Acceptor;
    /// Alias for network communication, namely (public key, private key) of the node
    KeyPair m_alias;
    /// Time we sent the last ping to all peers.
    std::chrono::steady_clock::time_point m_lastPing;
    /// Time we sent the last ping to all
    std::chrono::steady_clock::time_point m_lastReconnect;
    /// post and dispatch handlers
    boost::asio::io_service::strand m_strand;
    /// What port are we listening on. uint16_t(-1)
    /// means binding failed or acceptor hasn't been initialized.
    uint16_t m_listenPort = uint16_t(-1);
    /// representing to the network state
    bool m_run = false;
    ///< Start/stop mutex.(mutex for m_run)
    mutable std::mutex x_runTimer;
    /// the network accepting status(false means p2p is not accepting connections)
    bool m_accepting = false;
    /// maps from node ids to the sessions
    mutable std::unordered_map<NodeID, std::weak_ptr<SessionFace>> m_sessions;
    /// maps between peer name and peeer object
    /// attention: (endpoint recorded in m_peers always (listenIp, listenAddress))
    ///           (client endpoint of (connectIp, connectAddress) has no record)
    std::unordered_map<std::string, std::shared_ptr<Peer>> m_peers;
    /// mutex for accepting session information m_sessions and m_peers
    mutable RecursiveMutex x_sessions;
    /// Pending connections.
    std::list<std::weak_ptr<RLPXHandshake>> m_connecting;
    /// mutex for m_connecting
    Mutex x_connecting;
    /// control reconnecting, if set to be true, will reconnect peers immediately
    bool m_reconnectnow = true;
    /// mutex for m_reconnectnow
    Mutex x_reconnectnow;
    std::unique_ptr<boost::asio::deadline_timer> m_timer;
    /// Each of the capabilities we support
    std::map<CapDesc, std::shared_ptr<HostCapabilityFace>> m_capabilities;
    /// static nodes recording maps between endpoints of peers and node id
    std::map<NodeIPEndpoint, NodeID> m_staticNodes;
    /// public listening endpoint.
    bi::tcp::endpoint m_tcpPublic;
    // ip and port information of the connected peer
    bi::tcp::endpoint m_tcpClient;
    /// pending connections
    std::set<std::string> m_pendingPeerConns;
    /// mutex for m_pendingPeerConns
    Mutex x_pendingNodeConns;
    /// peer count limit
    unsigned m_maxPeerCount = 100;
    static const unsigned c_timerInterval = 100;

    shared_ptr<AsioInterface> m_asioInterface;
};
}  // namespace p2p

}  // namespace dev