// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The Bitcoin developers
// Copyright (c) 2011-2012 Bestcoin Developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "irc.h"
#include "db.h"
#include "net.h"
#include "init.h"
#include "strlcpy.h"
#include "addrman.h"
#include "ui_interface.h"

#ifdef WIN32
#include <string.h>
#endif

#ifdef USE_UPNP
#include <miniupnpc/miniwget.h>
#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>
#include <miniupnpc/upnperrors.h>
#endif

using namespace std;
using namespace boost;

static const int MAX_OUTBOUND_CONNECTIONS = 8;

void ThreadMessageHandler2(void* parg);
void ThreadSocketHandler2(void* parg);
void ThreadOpenConnections2(void* parg);
void ThreadOpenAddedConnections2(void* parg);
#ifdef USE_UPNP
void ThreadMapPort2(void* parg);
#endif
void ThreadDNSAddressSeed2(void* parg);
bool OpenNetworkConnection(const CAddress& addrConnect, CSemaphoreGrant *grantOutbound = NULL, const char *strDest = NULL, bool fOneShot = false);


struct LocalServiceInfo {
    int nScore;
    int nPort;
};

//
// Global state variables
//
bool fClient = false;
bool fDiscover = true;
bool fUseUPnP = false;
uint64 nLocalServices = (fClient ? 0 : NODE_NETWORK);
static CCriticalSection cs_mapLocalHost;
static map<CNetAddr, LocalServiceInfo> mapLocalHost;
static bool vfReachable[NET_MAX] = {};
static bool vfLimited[NET_MAX] = {};
static CNode* pnodeLocalHost = NULL;
uint64 nLocalHostNonce = 0;
array<int, THREAD_MAX> vnThreadsRunning;
static std::vector<SOCKET> vhListenSocket;
CAddrMan addrman;

vector<CNode*> vNodes;
CCriticalSection cs_vNodes;
map<CInv, CDataStream> mapRelay;
deque<pair<int64, CInv> > vRelayExpiration;
CCriticalSection cs_mapRelay;
map<CInv, int64> mapAlreadyAskedFor;

static deque<string> vOneShots;
CCriticalSection cs_vOneShots;

set<CNetAddr> setservAddNodeAddresses;
CCriticalSection cs_setservAddNodeAddresses;

static CSemaphore *semOutbound = NULL;

void AddOneShot(string strDest)
{
    LOCK(cs_vOneShots);
    vOneShots.push_back(strDest);
}

unsigned short GetListenPort()
{
    return (unsigned short)(GetArg("-port", GetDefaultPort()));
}

void CNode::PushGetBlocks(CBlockIndex* pindexBegin, uint256 hashEnd)
{
    // Filter out duplicate requests
    if (pindexBegin == pindexLastGetBlocksBegin && hashEnd == hashLastGetBlocksEnd)
        return;
    pindexLastGetBlocksBegin = pindexBegin;
    hashLastGetBlocksEnd = hashEnd;

    PushMessage("getblocks", CBlockLocator(pindexBegin), hashEnd);
}

// find 'best' local address for a particular peer
bool GetLocal(CService& addr, const CNetAddr *paddrPeer)
{
    if (fNoListen)
        return false;

    int nBestScore = -1;
    int nBestReachability = -1;
    {
        LOCK(cs_mapLocalHost);
        for (map<CNetAddr, LocalServiceInfo>::iterator it = mapLocalHost.begin(); it != mapLocalHost.end(); it++)
        {
            int nScore = (*it).second.nScore;
            int nReachability = (*it).first.GetReachabilityFrom(paddrPeer);
            if (nReachability > nBestReachability || (nReachability == nBestReachability && nScore > nBestScore))
            {
                addr = CService((*it).first, (*it).second.nPort);
                nBestReachability = nReachability;
                nBestScore = nScore;
            }
        }
    }
    return nBestScore >= 0;
}

// get best local address for a particular peer as a CAddress
CAddress GetLocalAddress(const CNetAddr *paddrPeer)
{
    CAddress ret(CService("0.0.0.0",0),0);
    CService addr;
    if (GetLocal(addr, paddrPeer))
    {
        ret = CAddress(addr);
        ret.nServices = nLocalServices;
        ret.nTime = GetAdjustedTime();
    }
    return ret;
}

bool RecvLine(SOCKET hSocket, string& strLine)
{
    strLine = "";
    loop
    {
        char c;
        int nBytes = recv(hSocket, &c, 1, 0);
        if (nBytes > 0)
        {
            if (c == '\n')
                continue;
            if (c == '\r')
                return true;
            strLine += c;
            if (strLine.size() >= 9000)
                return true;
        }
        else if (nBytes <= 0)
        {
            if (fShutdown)
                return false;
            if (nBytes < 0)
            {
                int nErr = WSAGetLastError();
                if (nErr == WSAEMSGSIZE)
                    continue;
                if (nErr == WSAEWOULDBLOCK || nErr == WSAEINTR || nErr == WSAEINPROGRESS)
                {
                    Sleep(10);
                    continue;
                }
            }
            if (!strLine.empty())
                return true;
            if (nBytes == 0)
            {
                // socket closed
                printf("socket closed\n");
                return false;
            }
            else
            {
                // socket error
                int nErr = WSAGetLastError();
                printf("recv failed: %d\n", nErr);
                return false;
            }
        }
    }
}

// used when scores of local addresses may have changed
// pushes better local address to peers
void static AdvertizeLocal()
{
    LOCK(cs_vNodes);
    BOOST_FOREACH(CNode* pnode, vNodes)
    {
        if (pnode->fSuccessfullyConnected)
        {
            CAddress addrLocal = GetLocalAddress(&pnode->addr);
            if (addrLocal.IsRoutable() && (CService)addrLocal != (CService)pnode->addrLocal)
            {
                pnode->PushAddress(addrLocal);
                pnode->addrLocal = addrLocal;
            }
        }
    }
}

void SetReachable(enum Network net, bool fFlag)
{
    LOCK(cs_mapLocalHost);
    vfReachable[net] = fFlag;
    if (net == NET_IPV6 && fFlag)
        vfReachable[NET_IPV4] = true;
}

// learn a new local address
bool AddLocal(const CService& addr, int nScore)
{
    if (!addr.IsRoutable())
        return false;

    if (!fDiscover && nScore < LOCAL_MANUAL)
        return false;

    if (IsLimited(addr))
        return false;

    printf("AddLocal(%s,%i)\n", addr.ToString().c_str(), nScore);

    {
        LOCK(cs_mapLocalHost);
        bool fAlready = mapLocalHost.count(addr) > 0;
        LocalServiceInfo &info = mapLocalHost[addr];
        if (!fAlready || nScore >= info.nScore) {
            info.nScore = nScore;
            info.nPort = addr.GetPort() + (fAlready ? 1 : 0);
        }
        SetReachable(addr.GetNetwork());
    }

    AdvertizeLocal();

    return true;
}

bool AddLocal(const CNetAddr &addr, int nScore)
{
    return AddLocal(CService(addr, GetListenPort()), nScore);
}

/** Make a particular network entirely off-limits (no automatic connects to it) */
void SetLimited(enum Network net, bool fLimited)
{
    if (net == NET_UNROUTABLE)
        return;
    LOCK(cs_mapLocalHost);
    vfLimited[net] = fLimited;
}

bool IsLimited(enum Network net)
{
    LOCK(cs_mapLocalHost);
    return vfLimited[net];
}

bool IsLimited(const CNetAddr &addr)
{
    return IsLimited(addr.GetNetwork());
}

/** vote for a local address */
bool SeenLocal(const CService& addr)
{
    {
        LOCK(cs_mapLocalHost);
        if (mapLocalHost.count(addr) == 0)
            return false;
        mapLocalHost[addr].nScore++;
    }

    AdvertizeLocal();

    return true;
}

/** check whether a given address is potentially local */
bool IsLocal(const CService& addr)
{
    LOCK(cs_mapLocalHost);
    return mapLocalHost.count(addr) > 0;
}

/** check whether a given address is in a network we can probably connect to */
bool IsReachable(const CNetAddr& addr)
{
    LOCK(cs_mapLocalHost);
    enum Network net = addr.GetNetwork();
    return vfReachable[net] && !vfLimited[net];
}

bool GetMyExternalIP2(const CService& addrConnect, const char* pszGet, const char* pszKeyword, CNetAddr& ipRet)
{
    SOCKET hSocket;
    if (!ConnectSocket(addrConnect, hSocket))
        return error("GetMyExternalIP() : connection to %s failed", addrConnect.ToString().c_str());

    send(hSocket, pszGet, strlen(pszGet), MSG_NOSIGNAL);

    string strLine;
    while (RecvLine(hSocket, strLine))
    {
        if (strLine.empty()) // HTTP response is separated from headers by blank line
        {
            loop
            {
                if (!RecvLine(hSocket, strLine))
                {
                    closesocket(hSocket);
                    return false;
                }
                if (pszKeyword == NULL)
                    break;
                if (strLine.find(pszKeyword) != string::npos)
                {
                    strLine = strLine.substr(strLine.find(pszKeyword) + strlen(pszKeyword));
                    break;
                }
            }
            closesocket(hSocket);
            if (strLine.find("<") != string::npos)
                strLine = strLine.substr(0, strLine.find("<"));
            strLine = strLine.substr(strspn(strLine.c_str(), " \t\n\r"));
            while (strLine.size() > 0 && isspace(strLine[strLine.size()-1]))
                strLine.resize(strLine.size()-1);
            CService addr(strLine,0,true);
            printf("GetMyExternalIP() received [%s] %s\n", strLine.c_str(), addr.ToString().c_str());
            if (!addr.IsValid() || !addr.IsRoutable())
                return false;
            ipRet.SetIP(addr);
            return true;
        }
    }
    closesocket(hSocket);
    return error("GetMyExternalIP() : connection closed");
}

// We now get our external IP from the IRC server first and only use this as a backup
bool GetMyExternalIP(CNetAddr& ipRet)
{
    CService addrConnect;
    const char* pszGet;
    const char* pszKeyword;

    for (int nLookup = 0; nLookup <= 1; nLookup++)
    for (int nHost = 1; nHost <= 2; nHost++)
    {
        // We should be phasing out our use of sites like these.  If we need
        // replacements, we should ask for volunteers to put this simple
        // php file on their webserver that prints the client IP:
        //  <?php echo $_SERVER["REMOTE_ADDR"]; ?>
        if (nHost == 1)
        {
            addrConnect = CService("91.198.22.70",80); // checkip.dyndns.org

            if (nLookup == 1)
            {
                CService addrIP("checkip.dyndns.org", 80, true);
                if (addrIP.IsValid())
                    addrConnect = addrIP;
            }

            pszGet = "GET / HTTP/1.1\r\n"
                     "Host: checkip.dyndns.org\r\n"
                     "User-Agent: Mozilla/4.0 (compatible; MSIE 7.0; Windows NT 5.1)\r\n"
                     "Connection: close\r\n"
                     "\r\n";

            pszKeyword = "Address:";
        }
        else if (nHost == 2)
        {
            addrConnect = CService("74.208.43.192", 80); // www.showmyip.com

            if (nLookup == 1)
            {
                CService addrIP("www.showmyip.com", 80, true);
                if (addrIP.IsValid())
                    addrConnect = addrIP;
            }

            pszGet = "GET /simple/ HTTP/1.1\r\n"
                     "Host: www.showmyip.com\r\n"
                     "User-Agent: Mozilla/4.0 (compatible; MSIE 7.0; Windows NT 5.1)\r\n"
                     "Connection: close\r\n"
                     "\r\n";

            pszKeyword = NULL; // Returns just IP address
        }

        if (GetMyExternalIP2(addrConnect, pszGet, pszKeyword, ipRet))
            return true;
    }

    return false;
}

void ThreadGetMyExternalIP(void* parg)
{
    // Make this thread recognisable as the external IP detection thread
    RenameThread("bitcoin-ext-ip");

    CNetAddr addrLocalHost;
    if (GetMyExternalIP(addrLocalHost))
    {
        printf("GetMyExternalIP() returned %s\n", addrLocalHost.ToStringIP().c_str());
        AddLocal(addrLocalHost, LOCAL_HTTP);
    }
}





void AddressCurrentlyConnected(const CService& addr)
{
    addrman.Connected(addr);
}







CNode* FindNode(const CNetAddr& ip)
{
    {
        LOCK(cs_vNodes);
        BOOST_FOREACH(CNode* pnode, vNodes)
            if ((CNetAddr)pnode->addr == ip)
                return (pnode);
    }
    return NULL;
}

CNode* FindNode(std::string addrName)
{
    LOCK(cs_vNodes);
    BOOST_FOREACH(CNode* pnode, vNodes)
        if (pnode->addrName == addrName)
            return (pnode);
    return NULL;
}

CNode* FindNode(const CService& addr)
{
    {
        LOCK(cs_vNodes);
        BOOST_FOREACH(CNode* pnode, vNodes)
            if ((CService)pnode->addr == addr)
                return (pnode);
    }
    return NULL;
}

CNode* ConnectNode(CAddress addrConnect, const char *pszDest, int64 nTimeout)
{
    if (pszDest == NULL) {
        if (IsLocal(addrConnect))
            return NULL;

        // Look for an existing connection
        CNode* pnode = FindNode((CService)addrConnect);
        if (pnode)
        {
            if (nTimeout != 0)
                pnode->AddRef(nTimeout);
            else
                pnode->AddRef();
            return pnode;
        }
    }


    /// debug print
    printf("trying connection %s lastseen=%.1fhrs\n",
        pszDest ? pszDest : addrConnect.ToString().c_str(),
        pszDest ? 0 : (double)(GetAdjustedTime() - addrConnect.nTime)/3600.0);

    // Connect
    SOCKET hSocket;
    if (pszDest ? ConnectSocketByName(addrConnect, hSocket, pszDest, GetDefaultPort()) : ConnectSocket(addrConnect, hSocket))
    {
        addrman.Attempt(addrConnect);

        /// debug print
        printf("connected %s\n", pszDest ? pszDest : addrConnect.ToString().c_str());

        // Set to nonblocking
#ifdef WIN32
        u_long nOne = 1;
        if (ioctlsocket(hSocket, FIONBIO, &nOne) == SOCKET_ERROR)
            printf("ConnectSocket() : ioctlsocket nonblocking setting failed, error %d\n", WSAGetLastError());
#else
        if (fcntl(hSocket, F_SETFL, O_NONBLOCK) == SOCKET_ERROR)
            printf("ConnectSocket() : fcntl nonblocking setting failed, error %d\n", errno);
#endif

        // Add node
        CNode* pnode = new CNode(hSocket, addrConnect, pszDest ? pszDest : "", false);
        if (nTimeout != 0)
            pnode->AddRef(nTimeout);
        else
            pnode->AddRef();

        {
            LOCK(cs_vNodes);
            vNodes.push_back(pnode);
        }

        pnode->nTimeConnected = GetTime();
        return pnode;
    }
    else
    {
        return NULL;
    }
}

void CNode::CloseSocketDisconnect()
{
    fDisconnect = true;
    if (hSocket != INVALID_SOCKET)
    {
        printf("disconnecting node %s\n", addrName.c_str());
        closesocket(hSocket);
        hSocket = INVALID_SOCKET;
        vRecv.clear();
    }
}

void CNode::Cleanup()
{
}


void CNode::PushVersion()
{
    /// when NTP implemented, change to just nTime = GetAdjustedTime()
    int64 nTime = (fInbound ? GetAdjustedTime() : GetTime());
    CAddress addrYou = (addr.IsRoutable() && !IsProxy(addr) ? addr : CAddress(CService("0.0.0.0",0)));
    CAddress addrMe = GetLocalAddress(&addr);
    RAND_bytes((unsigned char*)&nLocalHostNonce, sizeof(nLocalHostNonce));
    printf("send version message: version %d, blocks=%d, us=%s, them=%s, peer=%s\n", PROTOCOL_VERSION, nBestHeight, addrMe.ToString().c_str(), addrYou.ToString().c_str(), addr.ToString().c_str());
    PushMessage("version", PROTOCOL_VERSION, nLocalServices, nTime, addrYou, addrMe,
                nLocalHostNonce, FormatSubVersion(CLIENT_NAME, CLIENT_VERSION, std::vector<string>()), nBestHeight);
}





std::map<CNetAddr, int64> CNode::setBanned;
CCriticalSection CNode::cs_setBanned;

void CNode::ClearBanned()
{
    setBanned.clear();
}

bool CNode::IsBanned(CNetAddr ip)
{
    bool fResult = false;
    {
        LOCK(cs_setBanned);
        std::map<CNetAddr, int64>::iterator i = setBanned.find(ip);
        if (i != setBanned.end())
        {
            int64 t = (*i).second;
            if (GetTime() < t)
                fResult = true;
        }
    }
    return fResult;
}

bool CNode::Misbehaving(int howmuch)
{
    if (addr.IsLocal())
    {
        printf("Warning: local node %s misbehaving\n", addrName.c_str());
        return false;
    }

    nMisbehavior += howmuch;
    if (nMisbehavior >= GetArg("-banscore", 100))
    {
        int64 banTime = GetTime()+GetArg("-bantime", 60*60*24);  // Default 24-hour ban
        {
            LOCK(cs_setBanned);
            if (setBanned[addr] < banTime)
                setBanned[addr] = banTime;
        }
        CloseSocketDisconnect();
        printf("Disconnected %s for misbehavior (score=%d)\n", addrName.c_str(), nMisbehavior);
        return true;
    }
    return false;
}

#undef X
#define X(name) stats.name = name
void CNode::copyStats(CNodeStats &stats)
{
    X(nServices);
    X(nLastSend);
    X(nLastRecv);
    X(nTimeConnected);
    X(addrName);
    X(nVersion);
    X(strSubVer);
    X(fInbound);
    X(nReleaseTime);
    X(nStartingHeight);
    X(nMisbehavior);
}
#undef X










void ThreadSocketHandler(void* parg)
{
    IMPLEMENT_RANDOMIZE_STACK(ThreadSocketHandler(parg));

    // Make this thread recognisable as the networking thread
    RenameThread("bitcoin-net");

    try
    {
        vnThreadsRunning[THREAD_SOCKETHANDLER]++;
        ThreadSocketHandler2(parg);
        vnThreadsRunning[THREAD_SOCKETHANDLER]--;
    }
    catch (std::exception& e) {
        vnThreadsRunning[THREAD_SOCKETHANDLER]--;
        PrintException(&e, "ThreadSocketHandler()");
    } catch (...) {
        vnThreadsRunning[THREAD_SOCKETHANDLER]--;
        throw; // support pthread_cancel()
    }
    printf("ThreadSocketHandler exited\n");
}

void ThreadSocketHandler2(void* parg)
{
    printf("ThreadSocketHandler started\n");
    list<CNode*> vNodesDisconnected;
    unsigned int nPrevNodeCount = 0;

    loop
    {
        //
        // Disconnect nodes
        //
        {
            LOCK(cs_vNodes);
            // Disconnect unused nodes
            vector<CNode*> vNodesCopy = vNodes;
            BOOST_FOREACH(CNode* pnode, vNodesCopy)
            {
                if (pnode->fDisconnect ||
                    (pnode->GetRefCount() <= 0 && pnode->vRecv.empty() && pnode->vSend.empty()))
                {
                    // remove from vNodes
                    vNodes.erase(remove(vNodes.begin(), vNodes.end(), pnode), vNodes.end());

                    // release outbound grant (if any)
                    pnode->grantOutbound.Release();

                    // close socket and cleanup
                    pnode->CloseSocketDisconnect();
                    pnode->Cleanup();

                    // hold in disconnected pool until all refs are released
                    pnode->nReleaseTime = max(pnode->nReleaseTime, GetTime() + 15 * 60);
                    if (pnode->fNetworkNode || pnode->fInbound)
                        pnode->Release();
                    vNodesDisconnected.push_back(pnode);
                }
            }

            // Delete disconnected nodes
            list<CNode*> vNodesDisconnectedCopy = vNodesDisconnected;
            BOOST_FOREACH(CNode* pnode, vNodesDisconnectedCopy)
            {
                // wait until threads are done using it
                if (pnode->GetRefCount() <= 0)
                {
                    bool fDelete = false;
                    {
                        TRY_LOCK(pnode->cs_vSend, lockSend);
                        if (lockSend)
                        {
                            TRY_LOCK(pnode->cs_vRecv, lockRecv);
                            if (lockRecv)
                            {
                                TRY_LOCK(pnode->cs_mapRequests, lockReq);
                                if (lockReq)
                                {
                                    TRY_LOCK(pnode->cs_inventory, lockInv);
                                    if (lockInv)
                                        fDelete = true;
                                }
                            }
                        }
                    }
                    if (fDelete)
                    {
                        vNodesDisconnected.remove(pnode);
                        delete pnode;
                    }
                }
            }
        }
        if (vNodes.size() != nPrevNodeCount)
        {
            nPrevNodeCount = vNodes.size();
            uiInterface.NotifyNumConnectionsChanged(vNodes.size());
        }


        //
        // Find which sockets have data to receive
        //
        struct timeval timeout;
        timeout.tv_sec  = 0;
        timeout.tv_usec = 50000; // frequency to poll pnode->vSend

        fd_set fdsetRecv;
        fd_set fdsetSend;
        fd_set fdsetError;
        FD_ZERO(&fdsetRecv);
        FD_ZERO(&fdsetSend);
        FD_ZERO(&fdsetError);
        SOCKET hSocketMax = 0;

        BOOST_FOREACH(SOCKET hListenSocket, vhListenSocket) {
            FD_SET(hListenSocket, &fdsetRecv);
            hSocketMax = max(hSocketMax, hListenSocket);
        }
        {
            LOCK(cs_vNodes);
            BOOST_FOREACH(CNode* pnode, vNodes)
            {
                if (pnode->hSocket == INVALID_SOCKET)
                    continue;
                FD_SET(pnode->hSocket, &fdsetRecv);
                FD_SET(pnode->hSocket, &fdsetError);
                hSocketMax = max(hSocketMax, pnode->hSocket);
                {
                    TRY_LOCK(pnode->cs_vSend, lockSend);
                    if (lockSend && !pnode->vSend.empty())
                        FD_SET(pnode->hSocket, &fdsetSend);
                }
            }
        }

        vnThreadsRunning[THREAD_SOCKETHANDLER]--;
        int nSelect = select(hSocketMax + 1, &fdsetRecv, &fdsetSend, &fdsetError, &timeout);
        vnThreadsRunning[THREAD_SOCKETHANDLER]++;
        if (fShutdown)
            return;
        if (nSelect == SOCKET_ERROR)
        {
            int nErr = WSAGetLastError();
            if (hSocketMax != INVALID_SOCKET)
            {
                printf("socket select error %d\n", nErr);
                for (unsigned int i = 0; i <= hSocketMax; i++)
                    FD_SET(i, &fdsetRecv);
            }
            FD_ZERO(&fdsetSend);
            FD_ZERO(&fdsetError);
            Sleep(timeout.tv_usec/1000);
        }


        //
        // Accept new connections
        //
        BOOST_FOREACH(SOCKET hListenSocket, vhListenSocket)
        if (hListenSocket != INVALID_SOCKET && FD_ISSET(hListenSocket, &fdsetRecv))
        {
#ifdef USE_IPV6
            struct sockaddr_storage sockaddr;
#else
            struct sockaddr sockaddr;
#endif
            socklen_t len = sizeof(sockaddr);
            SOCKET hSocket = accept(hListenSocket, (struct sockaddr*)&sockaddr, &len);
            CAddress addr;
            int nInbound = 0;

            if (hSocket != INVALID_SOCKET)
                if (!addr.SetSockAddr((const struct sockaddr*)&sockaddr))
                    printf("warning: unknown socket family\n");

            {
                LOCK(cs_vNodes);
                BOOST_FOREACH(CNode* pnode, vNodes)
                    if (pnode->fInbound)
                        nInbound++;
            }

            if (hSocket == INVALID_SOCKET)
            {
                if (WSAGetLastError() != WSAEWOULDBLOCK)
                    printf("socket error accept failed: %d\n", WSAGetLastError());
            }
            else if (nInbound >= GetArg("-maxconnections", 125) - MAX_OUTBOUND_CONNECTIONS)
            {
                {
                    LOCK(cs_setservAddNodeAddresses);
                    if (!setservAddNodeAddresses.count(addr))
                        closesocket(hSocket);
                }
            }
            else if (CNode::IsBanned(addr))
            {
                printf("connection from %s dropped (banned)\n", addr.ToString().c_str());
                closesocket(hSocket);
            }
            else
            {
                printf("accepted connection %s\n", addr.ToString().c_str());
                CNode* pnode = new CNode(hSocket, addr, "", true);
                pnode->AddRef();
                {
                    LOCK(cs_vNodes);
                    vNodes.push_back(pnode);
                }
            }
        }


        //
        // Service each socket
        //
        vector<CNode*> vNodesCopy;
        {
            LOCK(cs_vNodes);
            vNodesCopy = vNodes;
            BOOST_FOREACH(CNode* pnode, vNodesCopy)
                pnode->AddRef();
        }
        BOOST_FOREACH(CNode* pnode, vNodesCopy)
        {
            if (fShutdown)
                return;

            //
            // Receive
            //
            if (pnode->hSocket == INVALID_SOCKET)
                continue;
            if (FD_ISSET(pnode->hSocket, &fdsetRecv) || FD_ISSET(pnode->hSocket, &fdsetError))
            {
                TRY_LOCK(pnode->cs_vRecv, lockRecv);
                if (lockRecv)
                {
                    CDataStream& vRecv = pnode->vRecv;
                    unsigned int nPos = vRecv.size();

                    if (nPos > ReceiveBufferSize()) {
                        if (!pnode->fDisconnect)
                            printf("socket recv flood control disconnect (%d bytes)\n", vRecv.size());
                        pnode->CloseSocketDisconnect();
                    }
                    else {
                        // typical socket buffer is 8K-64K
                        char pchBuf[0x10000];
                        int nBytes = recv(pnode->hSocket, pchBuf, sizeof(pchBuf), MSG_DONTWAIT);
                        if (nBytes > 0)
                        {
                            vRecv.resize(nPos + nBytes);
                            memcpy(&vRecv[nPos], pchBuf, nBytes);
                            pnode->nLastRecv = GetTime();
                        }
                        else if (nBytes == 0)
                        {
                            // socket closed gracefully
                            if (!pnode->fDisconnect)
                                printf("socket closed\n");
                            pnode->CloseSocketDisconnect();
                        }
                        else if (nBytes < 0)
                        {
                            // error
                            int nErr = WSAGetLastError();
                            if (nErr != WSAEWOULDBLOCK && nErr != WSAEMSGSIZE && nErr != WSAEINTR && nErr != WSAEINPROGRESS)
                            {
                                if (!pnode->fDisconnect)
                                    printf("socket recv error %d\n", nErr);
                                pnode->CloseSocketDisconnect();
                            }
                        }
                    }
                }
            }

            //
            // Send
            //
            if (pnode->hSocket == INVALID_SOCKET)
                continue;
            if (FD_ISSET(pnode->hSocket, &fdsetSend))
            {
                TRY_LOCK(pnode->cs_vSend, lockSend);
                if (lockSend)
                {
                    CDataStream& vSend = pnode->vSend;
                    if (!vSend.empty())
                    {
                        int nBytes = send(pnode->hSocket, &vSend[0], vSend.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
                        if (nBytes > 0)
                        {
                            vSend.erase(vSend.begin(), vSend.begin() + nBytes);
                            pnode->nLastSend = GetTime();
                        }
                        else if (nBytes < 0)
                        {
                            // error
                            int nErr = WSAGetLastError();
                            if (nErr != WSAEWOULDBLOCK && nErr != WSAEMSGSIZE && nErr != WSAEINTR && nErr != WSAEINPROGRESS)
                            {
                                printf("socket send error %d\n", nErr);
                                pnode->CloseSocketDisconnect();
                            }
                        }
                    }
                }
            }

            //
            // Inactivity checking
            //
            if (pnode->vSend.empty())
                pnode->nLastSendEmpty = GetTime();
            if (GetTime() - pnode->nTimeConnected > 60)
            {
                if (pnode->nLastRecv == 0 || pnode->nLastSend == 0)
                {
                    printf("socket no message in first 60 seconds, %d %d\n", pnode->nLastRecv != 0, pnode->nLastSend != 0);
                    pnode->fDisconnect = true;
                }
                else if (GetTime() - pnode->nLastSend > 90*60 && GetTime() - pnode->nLastSendEmpty > 90*60)
                {
                    printf("socket not sending\n");
                    pnode->fDisconnect = true;
                }
                else if (GetTime() - pnode->nLastRecv > 90*60)
                {
                    printf("socket inactivity timeout\n");
                    pnode->fDisconnect = true;
                }
            }
        }
        {
            LOCK(cs_vNodes);
            BOOST_FOREACH(CNode* pnode, vNodesCopy)
                pnode->Release();
        }

        Sleep(10);
    }
}









#ifdef USE_UPNP
void ThreadMapPort(void* parg)
{
    IMPLEMENT_RANDOMIZE_STACK(ThreadMapPort(parg));

    // Make this thread recognisable as the UPnP thread
    RenameThread("bitcoin-UPnP");

    try
    {
        vnThreadsRunning[THREAD_UPNP]++;
        ThreadMapPort2(parg);
        vnThreadsRunning[THREAD_UPNP]--;
    }
    catch (std::exception& e) {
        vnThreadsRunning[THREAD_UPNP]--;
        PrintException(&e, "ThreadMapPort()");
    } catch (...) {
        vnThreadsRunning[THREAD_UPNP]--;
        PrintException(NULL, "ThreadMapPort()");
    }
    printf("ThreadMapPort exited\n");
}

void ThreadMapPort2(void* parg)
{
    printf("ThreadMapPort started\n");

    char port[6];
    sprintf(port, "%d", GetListenPort());

    const char * multicastif = 0;
    const char * minissdpdpath = 0;
    struct UPNPDev * devlist = 0;
    char lanaddr[64];

#ifndef UPNPDISCOVER_SUCCESS
    /* miniupnpc 1.5 */
    devlist = upnpDiscover(2000, multicastif, minissdpdpath, 0);
#else
    /* miniupnpc 1.6 */
    int error = 0;
    devlist = upnpDiscover(2000, multicastif, minissdpdpath, 0, 0, &error);
#endif

    struct UPNPUrls urls;
    struct IGDdatas data;
    int r;

    r = UPNP_GetValidIGD(devlist, &urls, &data, lanaddr, sizeof(lanaddr));
    if (r == 1)
    {
        if (fDiscover) {
            char externalIPAddress[40];
            r = UPNP_GetExternalIPAddress(urls.controlURL, data.first.servicetype, externalIPAddress);
            if(r != UPNPCOMMAND_SUCCESS)
                printf("UPnP: GetExternalIPAddress() returned %d\n", r);
            else
            {
                if(externalIPAddress[0])
                {
                    printf("UPnP: ExternalIPAddress = %s\n", externalIPAddress);
                    AddLocal(CNetAddr(externalIPAddress), LOCAL_UPNP);
                }
                else
                    printf("UPnP: GetExternalIPAddress failed.\n");
            }
        }

        string strDesc = "Bestcoin " + FormatFullVersion();
#ifndef UPNPDISCOVER_SUCCESS
        /* miniupnpc 1.5 */
        r = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype,
                            port, port, lanaddr, strDesc.c_str(), "TCP", 0);
#else
        /* miniupnpc 1.6 */
        r = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype,
                            port, port, lanaddr, strDesc.c_str(), "TCP", 0, "0");
#endif

        if(r!=UPNPCOMMAND_SUCCESS)
            printf("AddPortMapping(%s, %s, %s) failed with code %d (%s)\n",
                port, port, lanaddr, r, strupnperror(r));
        else
            printf("UPnP Port Mapping successful.\n");
        int i = 1;
        loop {
            if (fShutdown || !fUseUPnP)
            {
                r = UPNP_DeletePortMapping(urls.controlURL, data.first.servicetype, port, "TCP", 0);
                printf("UPNP_DeletePortMapping() returned : %d\n", r);
                freeUPNPDevlist(devlist); devlist = 0;
                FreeUPNPUrls(&urls);
                return;
            }
            if (i % 600 == 0) // Refresh every 20 minutes
            {
#ifndef UPNPDISCOVER_SUCCESS
                /* miniupnpc 1.5 */
                r = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype,
                                    port, port, lanaddr, strDesc.c_str(), "TCP", 0);
#else
                /* miniupnpc 1.6 */
                r = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype,
                                    port, port, lanaddr, strDesc.c_str(), "TCP", 0, "0");
#endif

                if(r!=UPNPCOMMAND_SUCCESS)
                    printf("AddPortMapping(%s, %s, %s) failed with code %d (%s)\n",
                        port, port, lanaddr, r, strupnperror(r));
                else
                    printf("UPnP Port Mapping successful.\n");;
            }
            Sleep(2000);
            i++;
        }
    } else {
        printf("No valid UPnP IGDs found\n");
        freeUPNPDevlist(devlist); devlist = 0;
        if (r != 0)
            FreeUPNPUrls(&urls);
        loop {
            if (fShutdown || !fUseUPnP)
                return;
            Sleep(2000);
        }
    }
}

void MapPort()
{
    if (fUseUPnP && vnThreadsRunning[THREAD_UPNP] < 1)
    {
        if (!CreateThread(ThreadMapPort, NULL))
            printf("Error: ThreadMapPort(ThreadMapPort) failed\n");
    }
}
#else
void MapPort()
{
    // Intentionally left blank.
}
#endif

// DNS seeds
// Each pair gives a source name and a seed name.
// The first name is used as information source for addrman.
// The second name should resolve to a list of seed addresses.
static const char *strMainNetDNSSeed[][2] = {
    {"bestcointools.com", "dnsseed.bestcointools.com"},
    {"bestcoinpool.org", "dnsseed.bestcoinpool.org"},
    {"xurious.com", "dnsseed.ltc.xurious.com"},
    {"koin-project.com", "dnsseed.koin-project.com"},
    {"weminemnc.com", "dnsseed.weminemnc.com"},
    {NULL, NULL}
};

static const char *strTestNetDNSSeed[][2] = {
    {"bestcointools.com", "testnet-seed.bestcointools.com"},
    {"weminemnc.com", "testnet-seed.weminemnc.com"},
    {NULL, NULL}
};

void ThreadDNSAddressSeed(void* parg)
{
    IMPLEMENT_RANDOMIZE_STACK(ThreadDNSAddressSeed(parg));

    // Make this thread recognisable as the DNS seeding thread
    RenameThread("bitcoin-dnsseed");

    try
    {
        vnThreadsRunning[THREAD_DNSSEED]++;
        ThreadDNSAddressSeed2(parg);
        vnThreadsRunning[THREAD_DNSSEED]--;
    }
    catch (std::exception& e) {
        vnThreadsRunning[THREAD_DNSSEED]--;
        PrintException(&e, "ThreadDNSAddressSeed()");
    } catch (...) {
        vnThreadsRunning[THREAD_DNSSEED]--;
        throw; // support pthread_cancel()
    }
    printf("ThreadDNSAddressSeed exited\n");
}

void ThreadDNSAddressSeed2(void* parg)
{
    printf("ThreadDNSAddressSeed started\n");
    int found = 0;

    if (true) // minimize diff, this is the last version of 0.6.x
    {
        printf("Loading addresses from DNS seeds (could take a while)\n");

        static const char *(*strDNSSeed)[2] = fTestNet ? strTestNetDNSSeed : strMainNetDNSSeed;

        for (unsigned int seed_idx = 0; strDNSSeed[seed_idx][0] != NULL; seed_idx++) {
            if (GetNameProxy()) {
                AddOneShot(strDNSSeed[seed_idx][1]);
            } else {
                vector<CNetAddr> vaddr;
                vector<CAddress> vAdd;
                if (LookupHost(strDNSSeed[seed_idx][1], vaddr))
                {
                    BOOST_FOREACH(CNetAddr& ip, vaddr)
                    {
                        int nOneDay = 24*3600;
                        CAddress addr = CAddress(CService(ip, GetDefaultPort()));
                        addr.nTime = GetTime() - 3*nOneDay - GetRand(4*nOneDay); // use a random age between 3 and 7 days old
                        vAdd.push_back(addr);
                        found++;
                    }
                }
                addrman.Add(vAdd, CNetAddr(strDNSSeed[seed_idx][0], true));
            }
        }
    }

    printf("%d addresses found from DNS seeds\n", found);
}

unsigned int pnSeed[] =
{
    0xdfae795b, 0x0e0f46a6, 0xaf7f170c, 0x900486bc, 0xcbac226c, 0x3b551ac7, 0x56eb5d50, 0x136f21b2,
    0x8ddd57d0, 0xb291f363, 0xe57a3c47, 0x66c52560, 0x22b49640, 0xc76aaa1f, 0x5a90693e, 0x3534bdd5,
    0x37a06044, 0x43ecf65b, 0x4ce4995b, 0x7a563c43, 0x29d80c47, 0xe995316c, 0x1aeb864a, 0x3efc3d2a,
    0xf0e2a447, 0xaab216d2, 0xe74daa46, 0xa55fdcac, 0x2b8c811f, 0x625ec650, 0x2876a565, 0xfed55d50,
    0x9673bb48, 0xcd50d74a, 0xc048ed5e, 0xfe56ea47, 0x7e91a562, 0x6c6a888e, 0x07d55753, 0x8bb0bf42,
    0xf61bfd53, 0x1c26ea47, 0x849965ae, 0x96045d47, 0x0a4605be, 0x1f6a2a60, 0x8229fd18, 0x21cd11b8,
    0x9ea641ad, 0xad49d543, 0x45a6814a, 0x77ded690, 0x2dea1b52, 0xef73ad55, 0x8cc5614c, 0x43aca146,
    0x04c89851, 0xb130cb50, 0x686ff23c, 0x369a5f4b, 0x3cb58705, 0x54cd844b, 0x370d854f, 0xea812942,
    0xbc452864, 0xcef02a60, 0x6df6f854, 0xb5666251, 0x2d49386c, 0xb3892660, 0x7551f950, 0x0808a496,
    0x848954c6, 0x16f91ead, 0xd8d3834b, 0x7383794c, 0x8c68fc18, 0x7088066c, 0x5147b445, 0x87c052d8,
    0x4ab15cd4, 0xb8d53b44, 0xe5aa194c, 0x38bc6844, 0x5454c854, 0xfd091718, 0x7107a86c, 0x6454c854,
    0xc4bc8832, 0xfc67dcad, 0x091cb2c6, 0xada7ef47, 0xfcb3c05e, 0x0904f560, 0x37485253, 0x58aef763,
    0xfe716044, 0x1347eb63, 0xad435946, 0xb629d459, 0xf1785851, 0x6f3ec34a, 0xee079e4e, 0x515b7c46,
    0x1be31e98, 0x69f2e862, 0xd2d16f62, 0xb8cc9662, 0x340ad8b4, 0xdb15ee54, 0xaa81af51, 0xdfc57046,
    0xa44fbd42, 0x64f90456, 0x030dc65a, 0xd9842660, 0x03b6da18, 0x64786d4a, 0x8c8f338f, 0x0ec07acb,
    0xb4a5b34f, 0x3c815032, 0x330c1c71, 0x2abe814a, 0xdf0d62ae, 0x33e64979, 0x634dae58, 0xb14d40ad,
    0x948afe45, 0xf8545e4a, 0x9289f37b, 0x2c28a565, 0x25498232, 0x2dabed62, 0xb54df37b, 0x424a7d4c,
    0x1570456c, 0x0cf6bf42, 0x6e4a3f44, 0xa87158c3, 0x6910fb47, 0x29b8a247, 0xcb8d1518, 0x1d326596,
    0x3636dd50, 0xf9e5d762, 0x59d09a5e, 0x41a63e47, 0x5200a6b8, 0x38b71318, 0xc0c3456f, 0x02c6a43c,
    0xc92909b0, 0xa6b913d8, 0xe8b4e462, 0x6e30ac4c, 0x64439ca8, 0xdf8c5a18, 0xc24751d5, 0x5dc9fc6c,
    0xa9a15861, 0x87c4a73b, 0x243441ad, 0x2a853c42, 0x0188339a, 0xf3570f18, 0x0be83d44, 0xffa97257,
    0x23e026d0, 0x86b43760, 0x35ab664c, 0x22740f48, 0x6b6d4318, 0x9a1b1bd8, 0x12d0b64f, 0xd388b23e,
    0xd75cfddc, 0x8780cc47, 0x780e0e45, 0x2251cc82, 0x19fe8418, 0xefa5602a, 0xfb5f1074, 0xdc26ccc7,
    0xad5e7e4c, 0xb271922e, 0xb7054242, 0x9b9d7b47, 0xf9d4d862, 0x0de6e6d8, 0xf3dbcd48, 0x677fc3cc,
    0x7e6e1552, 0x40d69a18, 0x4cb1e152, 0xb4278c18, 0xa5be002e, 0xa95aac43, 0x6587454b, 0x513de262,
    0x15505a4e, 0x92434f46, 0x56522d79, 0x8453c918, 0x821b7544, 0xea69df50, 0xc7da4f5f, 0xcd12dc55,
    0x598a9147, 0x65ef7262, 0xdc454f46, 0x2119bc2e, 0xbedc7eae, 0x6ca8f536, 0x4497eb18, 0xe7ef80c3,
    0xd9a48e4b, 0x52946dcb, 0xcff428b2, 0x2eca1018, 0x4a685747, 0x77967045, 0x68801132, 0x9149ca62,
    0x04cc0844, 0xd65745b8, 0x4fb08218, 0x31362041, 0xa56c1d46, 0x89af1e47, 0x2d3fdb47, 0x1a3c146c,
    0xda1a694a, 0xf534c848, 0x8eee1eb7, 0xb7aac547, 0x1324f450, 0xaa88f745, 0x77e4aa8d, 0x8a1d52b8,
    0xb2499044, 0x26cb1918, 0x926d5c41, 0x4c218a4b, 0x27a90a4c, 0x1cbc8b59, 0x24ff7d4c, 0x391cc36d,
    0x0b58fddc, 0xc3ac0844, 0xe9d90ad8, 0x1b2f412e, 0x6a221b60, 0xe3518048, 0xc4071975, 0x1c3f72d8,
    0x1e8822d9, 0x067c7a76, 0x557cd782, 0xd5df0e43, 0x831a19bc, 0xe270ac55, 0x2106be5f, 0x49130087,
    0xdd1f0164, 0x76c75753, 0x2e935d02, 0x5491f460, 0x05736344, 0x748c3bda, 0x9534e95c, 0x64e9e444,
    0x96d55d61, 0xd9421656, 0xb044a143, 0xd2ca5a61, 0xbe074854, 0x48dadd43, 0x9cf29b18, 0x4641ee5c,
    0xa2581b53, 0xd585b86c, 0xa77c8a55, 0x5bd5aa47, 0x3b1a3947, 0x4cbcda51, 0x881a94b2, 0x463dc058,
    0xdc241618, 0x9f3b5d47, 0xdd431b4c, 0x76c9db57, 0x2dbfae47, 0x71a8fb53, 0x5447d562, 0x27f1e44e,
    0x0d7aa76c, 0x3e714945, 0x3fa9c544, 0xa6683532, 0x96e0fa45, 0xc4f8d782, 0xd6bddf62, 0xa1536d54,
    0x9404aa56, 0xca7bd742, 0x5ebc0164, 0x37737f45, 0x0fb8d2cc, 0x8dd4c04a, 0xdef43160, 0x71ff5053,
    0xbfe55d5f, 0x3539b34c, 0x90cdbeb8, 0xdb4e2247, 0x3bcc0b1f, 0xce5e5144, 0x7d7f57c7, 0xeeef3d55,
    0xea4a7752, 0x4a1906ae, 0x21e1cc6c, 0xe5b21cad, 0x10ab554b, 0xf553b147, 0xb796d162, 0x0aff2744,
    0x6b657054, 0x9115a665, 0xd873fb4d, 0x5abcd418, 0xccaf8ad5, 0x8dfb0718, 0x3bd6c248, 0xa581614c,
    0x18516041, 0xfa8c8e32, 0x96d33ea8, 0x0fed615f, 0x7b34c9dd, 0x928fa944, 0xb598b0ba, 0x9de9d562,
    0xa9c3bd4b, 0x5c275cae, 0xd61a8945, 0xcdc47f60, 0x0a46e962, 0x6596ed18, 0xae7ac46b, 0x5bcc4748,
    0x9ad6ba18, 0x1c428d4b, 0x9d0efa59, 0x68c006d1, 0xaedf5f41, 0x7caf0918, 0x48519b32, 0x091d1d46,
    0x36c0d176, 0xdb5be248, 0xc21d3b46, 0x18c61418, 0xd5a26044, 0x05898d18, 0xf11ee3be, 0xd10340ad,
    0xea33e218, 0xc4f3b742, 0xe5a50f18, 0x46f63b4a, 0xff72341f, 0x7c901dad, 0xc74f1162, 0xdf2b5032,
    0xc29b296c, 0x3c8eec5c, 0xfa8eaf6e, 0xc0bb5475, 0x4d2e1a46, 0x31f55a42, 0xa4fd211b, 0x8eed4147,
    0x506a3f18, 0x28521856, 0x88ccec47, 0xa8a92160, 0x4aa70e18, 0x1e644442, 0xc657ddd8, 0x368ce663,
    0xbe9859d1, 0x3e245ad4, 0xe6029744, 0x31de1f52, 0x4c617544, 0x945830ae, 0x5f82b543, 0xccfa574b,
    0xdee3b944, 0xb80bfcd8, 0x80b85741, 0x0980a545, 0x1c8dac62, 0x82ba6e62, 0x03c6ea42, 0x74c53b44,
    0xf9e02ebd, 0xcc3f715b, 0x942b0145, 0x9c8da545, 0x31980925, 0xcb9dd562, 0xf0ba0e4c, 0x9208564e,
    0x8ba74055, 0x96ed6c4b, 0x4057d74a, 0xc8d569d8, 0xfd48e4a5, 0x2f695670, 0x8387d418, 0x0902dd5f,
    0xf15aef47, 0x4368c65a, 0x2aeea988, 0xbdf6a94c, 0x7136165f, 0x86ea3dae, 0x8bbd1c6c, 0x9d2ed447,
    0x1987b59e, 0xc2bbb74f, 0x34b70b5c, 0xfaf65741, 0xab46e763, 0x33d454c6, 0xc26df645, 0x5f7351cf,
    0xf2ebefdc, 0xd4165c61, 0xafada73b, 0x68eaf7de, 0x0cee73d4, 0xc1753532, 0xda64f555, 0x6144406d,
    0x5eb0fb6c, 0x8993c90e, 0xab6d8792, 0x06d8a95f, 0xc60cfe45, 0xe3efb948, 0x0a393744, 0x8f47bcce,
    0x44b7d4b0, 0x3080fb4d, 0x2b95e744, 0xb010324d, 0xf820e247, 0x47384b80, 0xa54c41bc, 0x63a07618,
    0x004c4f5f, 0xde9aca47, 0xf9a842b8, 0x7208ae4b, 0x59927257, 0x2d1ea27c, 0x0386c65e, 0x3d0b6b3a,
    0x4ce6bd4b, 0x49ca67d5, 0xe1333bcb, 0x97684f5f, 0x54d130ad, 0x62470b25, 0xc11acab2, 0x6e7f947a,
    0x93f08056, 0x40588256, 0xf069e048, 0x4d224952, 0xcb846250, 0x4c9eaf6e, 0x7a2efe53, 0xa67ac3d1,
    0x64e3604a, 0xb5e154b2, 0x622a941f, 0xea76ca5c, 0x63f0515d, 0xe63efe62, 0x2ccbda72, 0x1c262b44,
    0x336fd956, 0x9d9f687a, 0x85d88856, 0xfc77be2e, 0x1c41bb7c, 0xf29c554d, 0x1268ee50, 0xd82d1abc,
    0x037a083a, 0xca5c8240, 0xf76fa73b, 0x2589494d, 0x3499fe4d, 0x67005553, 0xef8295b6, 0x21ca9662,
    0x4f8f1532, 0x480541b2, 0x0311d779, 0xb8b9a73b, 0xdb570a70, 0x93542dba, 0xde05a765, 0x23e863b0,
    0x69590352, 0xc8df0d56, 0x7e44db5d, 0x626aa97c, 0xf08b904f, 0x8be7063a, 0x288da94f, 0x7c9d1a41,
    0xae369478, 0xb1376c7a, 0x2b66cf18, 0xb021cf18, 0xafad957a, 0x8b69d62e, 0x32d3a87c, 0xc4ad846d,
    0x04c9888d, 0x418a3c02, 0x4f680471, 0x5881bd5e, 0x80bf0b1f, 0x569f966d, 0xae28fb5c, 0x25816c4a
};


void DumpAddresses()
{
    int64 nStart = GetTimeMillis();

    CAddrDB adb;
    adb.Write(addrman);

    printf("Flushed %d addresses to peers.dat  %"PRI64d"ms\n",
           addrman.size(), GetTimeMillis() - nStart);
}

void ThreadDumpAddress2(void* parg)
{
    vnThreadsRunning[THREAD_DUMPADDRESS]++;
    while (!fShutdown)
    {
        DumpAddresses();
        vnThreadsRunning[THREAD_DUMPADDRESS]--;
        Sleep(100000);
        vnThreadsRunning[THREAD_DUMPADDRESS]++;
    }
    vnThreadsRunning[THREAD_DUMPADDRESS]--;
}

void ThreadDumpAddress(void* parg)
{
    IMPLEMENT_RANDOMIZE_STACK(ThreadDumpAddress(parg));

    // Make this thread recognisable as the address dumping thread
    RenameThread("bitcoin-adrdump");

    try
    {
        ThreadDumpAddress2(parg);
    }
    catch (std::exception& e) {
        PrintException(&e, "ThreadDumpAddress()");
    }
    printf("ThreadDumpAddress exited\n");
}

void ThreadOpenConnections(void* parg)
{
    IMPLEMENT_RANDOMIZE_STACK(ThreadOpenConnections(parg));

    // Make this thread recognisable as the connection opening thread
    RenameThread("bitcoin-opencon");

    try
    {
        vnThreadsRunning[THREAD_OPENCONNECTIONS]++;
        ThreadOpenConnections2(parg);
        vnThreadsRunning[THREAD_OPENCONNECTIONS]--;
    }
    catch (std::exception& e) {
        vnThreadsRunning[THREAD_OPENCONNECTIONS]--;
        PrintException(&e, "ThreadOpenConnections()");
    } catch (...) {
        vnThreadsRunning[THREAD_OPENCONNECTIONS]--;
        PrintException(NULL, "ThreadOpenConnections()");
    }
    printf("ThreadOpenConnections exited\n");
}

void static ProcessOneShot()
{
    string strDest;
    {
        LOCK(cs_vOneShots);
        if (vOneShots.empty())
            return;
        strDest = vOneShots.front();
        vOneShots.pop_front();
    }
    CAddress addr;
    CSemaphoreGrant grant(*semOutbound, true);
    if (grant) {
        if (!OpenNetworkConnection(addr, &grant, strDest.c_str(), true))
            AddOneShot(strDest);
    }
}

void ThreadOpenConnections2(void* parg)
{
    printf("ThreadOpenConnections started\n");

    // Connect to specific addresses
    if (mapArgs.count("-connect"))
    {
        for (int64 nLoop = 0;; nLoop++)
        {
            ProcessOneShot();
            BOOST_FOREACH(string strAddr, mapMultiArgs["-connect"])
            {
                CAddress addr;
                OpenNetworkConnection(addr, NULL, strAddr.c_str());
                for (int i = 0; i < 10 && i < nLoop; i++)
                {
                    Sleep(500);
                    if (fShutdown)
                        return;
                }
            }
        }
    }

    // Initiate network connections
    int64 nStart = GetTime();
    loop
    {
        ProcessOneShot();

        vnThreadsRunning[THREAD_OPENCONNECTIONS]--;
        Sleep(500);
        vnThreadsRunning[THREAD_OPENCONNECTIONS]++;
        if (fShutdown)
            return;


        vnThreadsRunning[THREAD_OPENCONNECTIONS]--;
        CSemaphoreGrant grant(*semOutbound);
        vnThreadsRunning[THREAD_OPENCONNECTIONS]++;
        if (fShutdown)
            return;

        // Add seed nodes if IRC isn't working
        if (addrman.size()==0 && (GetTime() - nStart > 60) && !fTestNet)
        {
            std::vector<CAddress> vAdd;
            for (unsigned int i = 0; i < ARRAYLEN(pnSeed); i++)
            {
                // It'll only connect to one or two seed nodes because once it connects,
                // it'll get a pile of addresses with newer timestamps.
                // Seed nodes are given a random 'last seen time' of between one and two
                // weeks ago.
                const int64 nOneWeek = 7*24*60*60;
                struct in_addr ip;
                memcpy(&ip, &pnSeed[i], sizeof(ip));
                CAddress addr(CService(ip, GetDefaultPort()));
                addr.nTime = GetTime()-GetRand(nOneWeek)-nOneWeek;
                vAdd.push_back(addr);
            }
            addrman.Add(vAdd, CNetAddr("127.0.0.1"));
        }

        //
        // Choose an address to connect to based on most recently seen
        //
        CAddress addrConnect;

        // Only connect out to one peer per network group (/16 for IPv4).
        // Do this here so we don't have to critsect vNodes inside mapAddresses critsect.
        int nOutbound = 0;
        set<vector<unsigned char> > setConnected;
        {
            LOCK(cs_vNodes);
            BOOST_FOREACH(CNode* pnode, vNodes) {
                if (!pnode->fInbound) {
                    setConnected.insert(pnode->addr.GetGroup());
                    nOutbound++;
                }
            }
        }

        int64 nANow = GetAdjustedTime();

        int nTries = 0;
        loop
        {
            // use an nUnkBias between 10 (no outgoing connections) and 90 (8 outgoing connections)
            CAddress addr = addrman.Select(10 + min(nOutbound,8)*10);

            // if we selected an invalid address, restart
            if (!addr.IsValid() || setConnected.count(addr.GetGroup()) || IsLocal(addr))
                break;

            nTries++;

            if (IsLimited(addr))
                continue;

            // only consider very recently tried nodes after 30 failed attempts
            if (nANow - addr.nLastTry < 600 && nTries < 30)
                continue;

            // do not allow non-default ports, unless after 50 invalid addresses selected already
            if (addr.GetPort() != GetDefaultPort() && nTries < 50)
                continue;

            addrConnect = addr;
            break;
        }

        if (addrConnect.IsValid())
            OpenNetworkConnection(addrConnect, &grant);
    }
}

void ThreadOpenAddedConnections(void* parg)
{
    IMPLEMENT_RANDOMIZE_STACK(ThreadOpenAddedConnections(parg));

    // Make this thread recognisable as the connection opening thread
    RenameThread("bitcoin-opencon");

    try
    {
        vnThreadsRunning[THREAD_ADDEDCONNECTIONS]++;
        ThreadOpenAddedConnections2(parg);
        vnThreadsRunning[THREAD_ADDEDCONNECTIONS]--;
    }
    catch (std::exception& e) {
        vnThreadsRunning[THREAD_ADDEDCONNECTIONS]--;
        PrintException(&e, "ThreadOpenAddedConnections()");
    } catch (...) {
        vnThreadsRunning[THREAD_ADDEDCONNECTIONS]--;
        PrintException(NULL, "ThreadOpenAddedConnections()");
    }
    printf("ThreadOpenAddedConnections exited\n");
}

void ThreadOpenAddedConnections2(void* parg)
{
    printf("ThreadOpenAddedConnections started\n");

    if (mapArgs.count("-addnode") == 0)
        return;

    if (GetNameProxy()) {
        while(!fShutdown) {
            BOOST_FOREACH(string& strAddNode, mapMultiArgs["-addnode"]) {
                CAddress addr;
                CSemaphoreGrant grant(*semOutbound);
                OpenNetworkConnection(addr, &grant, strAddNode.c_str());
                Sleep(500);
            }
            vnThreadsRunning[THREAD_ADDEDCONNECTIONS]--;
            Sleep(120000); // Retry every 2 minutes
            vnThreadsRunning[THREAD_ADDEDCONNECTIONS]++;
        }
        return;
    }

    vector<vector<CService> > vservAddressesToAdd(0);
    BOOST_FOREACH(string& strAddNode, mapMultiArgs["-addnode"])
    {
        vector<CService> vservNode(0);
        if(Lookup(strAddNode.c_str(), vservNode, GetDefaultPort(), fNameLookup, 0))
        {
            vservAddressesToAdd.push_back(vservNode);
            {
                LOCK(cs_setservAddNodeAddresses);
                BOOST_FOREACH(CService& serv, vservNode)
                    setservAddNodeAddresses.insert(serv);
            }
        }
    }
    loop
    {
        vector<vector<CService> > vservConnectAddresses = vservAddressesToAdd;
        // Attempt to connect to each IP for each addnode entry until at least one is successful per addnode entry
        // (keeping in mind that addnode entries can have many IPs if fNameLookup)
        {
            LOCK(cs_vNodes);
            BOOST_FOREACH(CNode* pnode, vNodes)
                for (vector<vector<CService> >::iterator it = vservConnectAddresses.begin(); it != vservConnectAddresses.end(); it++)
                    BOOST_FOREACH(CService& addrNode, *(it))
                        if (pnode->addr == addrNode)
                        {
                            it = vservConnectAddresses.erase(it);
                            it--;
                            break;
                        }
        }
        BOOST_FOREACH(vector<CService>& vserv, vservConnectAddresses)
        {
            CSemaphoreGrant grant(*semOutbound);
            OpenNetworkConnection(CAddress(*(vserv.begin())), &grant);
            Sleep(500);
            if (fShutdown)
                return;
        }
        if (fShutdown)
            return;
        vnThreadsRunning[THREAD_ADDEDCONNECTIONS]--;
        Sleep(120000); // Retry every 2 minutes
        vnThreadsRunning[THREAD_ADDEDCONNECTIONS]++;
        if (fShutdown)
            return;
    }
}

// if succesful, this moves the passed grant to the constructed node
bool OpenNetworkConnection(const CAddress& addrConnect, CSemaphoreGrant *grantOutbound, const char *strDest, bool fOneShot)
{
    //
    // Initiate outbound network connection
    //
    if (fShutdown)
        return false;
    if (!strDest)
        if (IsLocal(addrConnect) ||
            FindNode((CNetAddr)addrConnect) || CNode::IsBanned(addrConnect) ||
            FindNode(addrConnect.ToStringIPPort().c_str()))
            return false;
    if (strDest && FindNode(strDest))
        return false;

    vnThreadsRunning[THREAD_OPENCONNECTIONS]--;
    CNode* pnode = ConnectNode(addrConnect, strDest);
    vnThreadsRunning[THREAD_OPENCONNECTIONS]++;
    if (fShutdown)
        return false;
    if (!pnode)
        return false;
    if (grantOutbound)
        grantOutbound->MoveTo(pnode->grantOutbound);
    pnode->fNetworkNode = true;
    if (fOneShot)
        pnode->fOneShot = true;

    return true;
}








void ThreadMessageHandler(void* parg)
{
    IMPLEMENT_RANDOMIZE_STACK(ThreadMessageHandler(parg));

    // Make this thread recognisable as the message handling thread
    RenameThread("bitcoin-msghand");

    try
    {
        vnThreadsRunning[THREAD_MESSAGEHANDLER]++;
        ThreadMessageHandler2(parg);
        vnThreadsRunning[THREAD_MESSAGEHANDLER]--;
    }
    catch (std::exception& e) {
        vnThreadsRunning[THREAD_MESSAGEHANDLER]--;
        PrintException(&e, "ThreadMessageHandler()");
    } catch (...) {
        vnThreadsRunning[THREAD_MESSAGEHANDLER]--;
        PrintException(NULL, "ThreadMessageHandler()");
    }
    printf("ThreadMessageHandler exited\n");
}

void ThreadMessageHandler2(void* parg)
{
    printf("ThreadMessageHandler started\n");
    SetThreadPriority(THREAD_PRIORITY_BELOW_NORMAL);
    while (!fShutdown)
    {
        vector<CNode*> vNodesCopy;
        {
            LOCK(cs_vNodes);
            vNodesCopy = vNodes;
            BOOST_FOREACH(CNode* pnode, vNodesCopy)
                pnode->AddRef();
        }

        // Poll the connected nodes for messages
        CNode* pnodeTrickle = NULL;
        if (!vNodesCopy.empty())
            pnodeTrickle = vNodesCopy[GetRand(vNodesCopy.size())];
        BOOST_FOREACH(CNode* pnode, vNodesCopy)
        {
            // Receive messages
            {
                TRY_LOCK(pnode->cs_vRecv, lockRecv);
                if (lockRecv)
                    ProcessMessages(pnode);
            }
            if (fShutdown)
                return;

            // Send messages
            {
                TRY_LOCK(pnode->cs_vSend, lockSend);
                if (lockSend)
                    SendMessages(pnode, pnode == pnodeTrickle);
            }
            if (fShutdown)
                return;
        }

        {
            LOCK(cs_vNodes);
            BOOST_FOREACH(CNode* pnode, vNodesCopy)
                pnode->Release();
        }

        // Wait and allow messages to bunch up.
        // Reduce vnThreadsRunning so StopNode has permission to exit while
        // we're sleeping, but we must always check fShutdown after doing this.
        vnThreadsRunning[THREAD_MESSAGEHANDLER]--;
        Sleep(100);
        if (fRequestShutdown)
            StartShutdown();
        vnThreadsRunning[THREAD_MESSAGEHANDLER]++;
        if (fShutdown)
            return;
    }
}






bool BindListenPort(const CService &addrBind, string& strError)
{
    strError = "";
    int nOne = 1;

#ifdef WIN32
    // Initialize Windows Sockets
    WSADATA wsadata;
    int ret = WSAStartup(MAKEWORD(2,2), &wsadata);
    if (ret != NO_ERROR)
    {
        strError = strprintf("Error: TCP/IP socket library failed to start (WSAStartup returned error %d)", ret);
        printf("%s\n", strError.c_str());
        return false;
    }
#endif

    // Create socket for listening for incoming connections
#ifdef USE_IPV6
    struct sockaddr_storage sockaddr;
#else
    struct sockaddr sockaddr;
#endif
    socklen_t len = sizeof(sockaddr);
    if (!addrBind.GetSockAddr((struct sockaddr*)&sockaddr, &len))
    {
        strError = strprintf("Error: bind address family for %s not supported", addrBind.ToString().c_str());
        printf("%s\n", strError.c_str());
        return false;
    }

    SOCKET hListenSocket = socket(((struct sockaddr*)&sockaddr)->sa_family, SOCK_STREAM, IPPROTO_TCP);
    if (hListenSocket == INVALID_SOCKET)
    {
        strError = strprintf("Error: Couldn't open socket for incoming connections (socket returned error %d)", WSAGetLastError());
        printf("%s\n", strError.c_str());
        return false;
    }

#ifdef SO_NOSIGPIPE
    // Different way of disabling SIGPIPE on BSD
    setsockopt(hListenSocket, SOL_SOCKET, SO_NOSIGPIPE, (void*)&nOne, sizeof(int));
#endif

#ifndef WIN32
    // Allow binding if the port is still in TIME_WAIT state after
    // the program was closed and restarted.  Not an issue on windows.
    setsockopt(hListenSocket, SOL_SOCKET, SO_REUSEADDR, (void*)&nOne, sizeof(int));
#endif


#ifdef WIN32
    // Set to nonblocking, incoming connections will also inherit this
    if (ioctlsocket(hListenSocket, FIONBIO, (u_long*)&nOne) == SOCKET_ERROR)
#else
    if (fcntl(hListenSocket, F_SETFL, O_NONBLOCK) == SOCKET_ERROR)
#endif
    {
        strError = strprintf("Error: Couldn't set properties on socket for incoming connections (error %d)", WSAGetLastError());
        printf("%s\n", strError.c_str());
        return false;
    }

#ifdef USE_IPV6
    // some systems don't have IPV6_V6ONLY but are always v6only; others do have the option
    // and enable it by default or not. Try to enable it, if possible.
    if (addrBind.IsIPv6()) {
#ifdef IPV6_V6ONLY
        setsockopt(hListenSocket, IPPROTO_IPV6, IPV6_V6ONLY, (void*)&nOne, sizeof(int));
#endif
#ifdef WIN32
        int nProtLevel = 10 /* PROTECTION_LEVEL_UNRESTRICTED */;
        int nParameterId = 23 /* IPV6_PROTECTION_LEVEl */;
        // this call is allowed to fail
        setsockopt(hListenSocket, IPPROTO_IPV6, nParameterId, (const char*)&nProtLevel, sizeof(int));
#endif
    }
#endif

    if (::bind(hListenSocket, (struct sockaddr*)&sockaddr, len) == SOCKET_ERROR)
    {
        int nErr = WSAGetLastError();
        if (nErr == WSAEADDRINUSE)
            strError = strprintf(_("Unable to bind to %s on this computer. Bestcoin is probably already running."), addrBind.ToString().c_str());
        else
            strError = strprintf(_("Unable to bind to %s on this computer (bind returned error %d, %s)"), addrBind.ToString().c_str(), nErr, strerror(nErr));
        printf("%s\n", strError.c_str());
        return false;
    }
    printf("Bound to %s\n", addrBind.ToString().c_str());

    // Listen for incoming connections
    if (listen(hListenSocket, SOMAXCONN) == SOCKET_ERROR)
    {
        strError = strprintf("Error: Listening for incoming connections failed (listen returned error %d)", WSAGetLastError());
        printf("%s\n", strError.c_str());
        return false;
    }

    vhListenSocket.push_back(hListenSocket);

    if (addrBind.IsRoutable() && fDiscover)
        AddLocal(addrBind, LOCAL_BIND);

    return true;
}

void static Discover()
{
    if (!fDiscover)
        return;

#ifdef WIN32
    // Get local host ip
    char pszHostName[1000] = "";
    if (gethostname(pszHostName, sizeof(pszHostName)) != SOCKET_ERROR)
    {
        vector<CNetAddr> vaddr;
        if (LookupHost(pszHostName, vaddr))
        {
            BOOST_FOREACH (const CNetAddr &addr, vaddr)
            {
                AddLocal(addr, LOCAL_IF);
            }
        }
    }
#else
    // Get local host ip
    struct ifaddrs* myaddrs;
    if (getifaddrs(&myaddrs) == 0)
    {
        for (struct ifaddrs* ifa = myaddrs; ifa != NULL; ifa = ifa->ifa_next)
        {
            if (ifa->ifa_addr == NULL) continue;
            if ((ifa->ifa_flags & IFF_UP) == 0) continue;
            if (strcmp(ifa->ifa_name, "lo") == 0) continue;
            if (strcmp(ifa->ifa_name, "lo0") == 0) continue;
            if (ifa->ifa_addr->sa_family == AF_INET)
            {
                struct sockaddr_in* s4 = (struct sockaddr_in*)(ifa->ifa_addr);
                CNetAddr addr(s4->sin_addr);
                if (AddLocal(addr, LOCAL_IF))
                    printf("IPv4 %s: %s\n", ifa->ifa_name, addr.ToString().c_str());
            }
#ifdef USE_IPV6
            else if (ifa->ifa_addr->sa_family == AF_INET6)
            {
                struct sockaddr_in6* s6 = (struct sockaddr_in6*)(ifa->ifa_addr);
                CNetAddr addr(s6->sin6_addr);
                if (AddLocal(addr, LOCAL_IF))
                    printf("IPv6 %s: %s\n", ifa->ifa_name, addr.ToString().c_str());
            }
#endif
        }
        freeifaddrs(myaddrs);
    }
#endif

    CreateThread(ThreadGetMyExternalIP, NULL);
}

void StartNode(void* parg)
{
    // Make this thread recognisable as the startup thread
    RenameThread("bitcoin-start");

    if (semOutbound == NULL) {
        // initialize semaphore
        int nMaxOutbound = min(MAX_OUTBOUND_CONNECTIONS, (int)GetArg("-maxconnections", 125));
        semOutbound = new CSemaphore(nMaxOutbound);
    }

    if (pnodeLocalHost == NULL)
        pnodeLocalHost = new CNode(INVALID_SOCKET, CAddress(CService("127.0.0.1", 0), nLocalServices));

    Discover();

    //
    // Start threads
    //

    if (!GetBoolArg("-dnsseed", true))
        printf("DNS seeding disabled\n");
    else
        if (!CreateThread(ThreadDNSAddressSeed, NULL))
            printf("Error: CreateThread(ThreadDNSAddressSeed) failed\n");

    // Map ports with UPnP
    if (fUseUPnP)
        MapPort();

    // Get addresses from IRC and advertise ours
    if (!CreateThread(ThreadIRCSeed, NULL))
        printf("Error: CreateThread(ThreadIRCSeed) failed\n");

    // Send and receive from sockets, accept connections
    if (!CreateThread(ThreadSocketHandler, NULL))
        printf("Error: CreateThread(ThreadSocketHandler) failed\n");

    // Initiate outbound connections from -addnode
    if (!CreateThread(ThreadOpenAddedConnections, NULL))
        printf("Error: CreateThread(ThreadOpenAddedConnections) failed\n");

    // Initiate outbound connections
    if (!CreateThread(ThreadOpenConnections, NULL))
        printf("Error: CreateThread(ThreadOpenConnections) failed\n");

    // Process messages
    if (!CreateThread(ThreadMessageHandler, NULL))
        printf("Error: CreateThread(ThreadMessageHandler) failed\n");

    // Dump network addresses
    if (!CreateThread(ThreadDumpAddress, NULL))
        printf("Error; CreateThread(ThreadDumpAddress) failed\n");
}

bool StopNode()
{
    printf("StopNode()\n");
    fShutdown = true;
    nTransactionsUpdated++;
    int64 nStart = GetTime();
    if (semOutbound)
        for (int i=0; i<MAX_OUTBOUND_CONNECTIONS; i++)
            semOutbound->post();
    do
    {
        int nThreadsRunning = 0;
        for (int n = 0; n < THREAD_MAX; n++)
            nThreadsRunning += vnThreadsRunning[n];
        if (nThreadsRunning == 0)
            break;
        if (GetTime() - nStart > 20)
            break;
        Sleep(20);
    } while(true);
    if (vnThreadsRunning[THREAD_SOCKETHANDLER] > 0) printf("ThreadSocketHandler still running\n");
    if (vnThreadsRunning[THREAD_OPENCONNECTIONS] > 0) printf("ThreadOpenConnections still running\n");
    if (vnThreadsRunning[THREAD_MESSAGEHANDLER] > 0) printf("ThreadMessageHandler still running\n");
    if (vnThreadsRunning[THREAD_MINER] > 0) printf("ThreadBitcoinMiner still running\n");
    if (vnThreadsRunning[THREAD_RPCLISTENER] > 0) printf("ThreadRPCListener still running\n");
    if (vnThreadsRunning[THREAD_RPCHANDLER] > 0) printf("ThreadsRPCServer still running\n");
#ifdef USE_UPNP
    if (vnThreadsRunning[THREAD_UPNP] > 0) printf("ThreadMapPort still running\n");
#endif
    if (vnThreadsRunning[THREAD_DNSSEED] > 0) printf("ThreadDNSAddressSeed still running\n");
    if (vnThreadsRunning[THREAD_ADDEDCONNECTIONS] > 0) printf("ThreadOpenAddedConnections still running\n");
    if (vnThreadsRunning[THREAD_DUMPADDRESS] > 0) printf("ThreadDumpAddresses still running\n");
    while (vnThreadsRunning[THREAD_MESSAGEHANDLER] > 0 || vnThreadsRunning[THREAD_RPCHANDLER] > 0)
        Sleep(20);
    Sleep(50);
    DumpAddresses();
    return true;
}

class CNetCleanup
{
public:
    CNetCleanup()
    {
    }
    ~CNetCleanup()
    {
        // Close sockets
        BOOST_FOREACH(CNode* pnode, vNodes)
            if (pnode->hSocket != INVALID_SOCKET)
                closesocket(pnode->hSocket);
        BOOST_FOREACH(SOCKET hListenSocket, vhListenSocket)
            if (hListenSocket != INVALID_SOCKET)
                if (closesocket(hListenSocket) == SOCKET_ERROR)
                    printf("closesocket(hListenSocket) failed with error %d\n", WSAGetLastError());

#ifdef WIN32
        // Shutdown Windows Sockets
        WSACleanup();
#endif
    }
}
instance_of_cnetcleanup;
