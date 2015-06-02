#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <algorithm>
#include <string.h>

#include "ssdp.h"

#include "array.h"
#include "dstrings.h"
#include "socket.h"
#include "hash.h"
#include "hashtable.h"
#include "time.h"
#include "log.h"
#include "template.h"
#include "http_server_private.h"
#include "http_client_private.h"
#include "http_server.h"

namespace dmSSDP
{
    const char * SSDP_MCAST_ADDR = "239.255.255.250";
    const uint16_t SSDP_MCAST_PORT = 1900U;
    const uint32_t SSDP_MCAST_TTL = 4U;

    static const char* SSDP_ALIVE_TMPL =
        "NOTIFY * HTTP/1.1\r\n"
        "SERVER: Defold SSDP 1.0\r\n"
        "CACHE-CONTROL: max-age=${MAX_AGE}\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "LOCATION: http://${HOSTNAME}:${HTTPPORT}/${ID}\r\n"
        "NTS: ssdp:alive\r\n"
        "NT: ${NT}\r\n"
        "USN: ${UDN}::${DEVICE_TYPE}\r\n\r\n";

    static const char* SSDP_BYEBYE_TMPL =
        "NOTIFY * HTTP/1.1\r\n"
        "SERVER: Defold SSDP 1.0\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "NTS: ssdp:byebye\r\n"
        "NT: ${NT}\r\n"
        "USN: ${UDN}::${DEVICE_TYPE}\r\n\r\n";

    /*
     * NOTE: We skip the following recommended headers (no time/data api in dlib)
     * - DATE
     */
    static const char* SEARCH_RESULT_FMT =
        "HTTP/1.1 200 OK\r\n"
        "SERVER: Defold SSDP 1.0\r\n"
        "CACHE-CONTROL: max-age=${MAX_AGE}\r\n"
        "LOCATION: http://${HOSTNAME}:${HTTPPORT}/${ID}\r\n"
        "ST: ${ST}\r\n"
        "EXT:\r\n"
        "USN: ${UDN}::${DEVICE_TYPE}\r\n"
        "Content-Length: 0\r\n\r\n";

    static const char * M_SEARCH_FMT =
        "M-SEARCH * HTTP/1.1\r\n"
        "SERVER: Defold SSDP 1.0\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        "MX: 3\r\n"
        "ST: upnp:rootdevice\r\n\r\n";

    const uint32_t SSDP_LOCAL_ADDRESS_EXPIRATION = 4U;
    const uint32_t SSDP_MAX_LOCAL_ADDRESSES = 32U;

    struct Device
    {
        // Only available for registered devices
        const DeviceDesc*   m_DeviceDesc;

        struct IfAddrState {
            uint64_t m_Expires;
            dmSocket::Address m_Address;
        } m_IfAddrState[SSDP_MAX_LOCAL_ADDRESSES];

        uint32_t m_IfAddrStateCount;

        // Time when the device expires
        // For registered devices: a notification should be sent
        // For discovered devices: the device should be removed
        uint64_t            m_Expires;

        Device()
        {
            memset(this, 0, sizeof(*this));
        }

        Device(const DeviceDesc* device_desc)
        {
            memset(this, 0, sizeof(*this));
            m_DeviceDesc = device_desc;
            // NOTE: We set expires to such that announce messages
            // will be sent in one second (if enabled)
            m_Expires = dmTime::GetTime();
        }
    };

    struct SSDP
    {
        SSDP()
        {
            memset(this, 0, sizeof(*this));
            m_DiscoveredDevices.SetCapacity(983, 1024);
            m_RegistredEntries.SetCapacity(17, 32);
            m_MCastSocket = dmSocket::INVALID_SOCKET_HANDLE;
        }

        // Max age for registered devices
        uint32_t                m_MaxAge;
        char                    m_MaxAgeText[16];

        // True if announce messages should be sent
        uint32_t                m_Announce : 1;

        // Time interval for annouce messages (if m_Announce is set). Typically m_MaxAge / 2
        uint32_t                m_AnnounceInterval;

        // True if reconnection should be performed in next update
        uint32_t                m_Reconnect : 1;

        // Send/Receive buffer
        uint8_t                 m_Buffer[1500];

        // All discovered devices
        dmHashTable64<Device>   m_DiscoveredDevices;

        // All registered devices
        dmHashTable64<Device*>  m_RegistredEntries;

        // Port for m_Socket
        uint16_t                m_Port;

        // Socket for multicast receive
        dmSocket::Socket        m_MCastSocket;

        // Addresses & sockets for each available network address.
        // Mutex lock is held during Update() calls and from addr. update thread.
        dmSocket::IfAddr        m_LocalAddr[SSDP_MAX_LOCAL_ADDRESSES];
        dmSocket::Socket        m_LocalAddrSocket[SSDP_MAX_LOCAL_ADDRESSES];
        uint32_t                m_LocalAddrCount;
        uint64_t                m_LocalAddrExpires;

        // Hostname for the current request being processed.
        char                    m_HttpHost[64];

        // Http server for device descriptions
        dmHttpServer::HServer   m_HttpServer;
        char                    m_HttpPortText[8];
    };

    struct Replacer
    {
        Replacer*                   m_Parent;
        void*                       m_Userdata;
        dmTemplate::ReplaceCallback m_Callback;

        Replacer(Replacer* parent, void* user_data, dmTemplate::ReplaceCallback call_back)
        {
            m_Parent = parent;
            m_Userdata = user_data;
            m_Callback = call_back;
        }

        static const char* Replace(void* user_data, const char* key);
    };

    const char* Replacer::Replace(void* user_data, const char* key)
    {
        Replacer* self = (Replacer*) user_data;
        const char* value = self->m_Callback(self->m_Userdata, key);
        if (value)
            return value;
        else if (self->m_Parent)
            return Replacer::Replace(self->m_Parent, key);
        else
            return 0;
    }

    struct ReplaceContext
    {
        SSDP*   m_SSDP;
        Device* m_Device;

        ReplaceContext(SSDP* ssdp, Device* device)
        {
            m_SSDP = ssdp;
            m_Device = device;
        }
    };

    enum RequestType
    {
        RT_UNKNOWN    = 0,
        RT_NOTIFY     = 1,
        RT_M_SEARCH   = 2,
    };

    struct RequestParseState
    {
        SSDP*                       m_SSDP;
        // Parsed max-age
        uint32_t                    m_MaxAge;

        // Request-type, ie NOTIFY or M-SEARCH
        RequestType                 m_RequestType;

        // All headers
        dmHashTable64<const char*>  m_Headers;

        // HTTP status
        int                         m_Status;

        // Notification Type (NT)
        dmhash_t                    m_NTHash;
        // Notification Sub Type (NTS)
        dmhash_t                    m_NTSHash;

        RequestParseState(SSDP* ssdp)
        {
            memset(this, 0, sizeof(*this));
            m_SSDP = ssdp;
            m_Headers.SetCapacity(27, 64);
            // Default max-age if none is found
            m_MaxAge = 1800;
        }

        static void FreeCallback(RequestParseState *state, const dmhash_t* key, const char** value);

        ~RequestParseState()
        {
            m_Headers.Iterate(FreeCallback, this);
        }
    };

    struct SearchResponseContext
    {
        RequestParseState*  m_State;
        const char*         m_ST;
        dmSocket::Address   m_FromAddress;
        uint16_t            m_FromPort;

        SearchResponseContext(RequestParseState* state, const char* st, dmSocket::Address from_address, uint16_t from_port)
        {
            m_State = state;
            m_ST = st;
            m_FromAddress = from_address;
            m_FromPort = from_port;
        }
    };

    static const char* ReplaceIfAddrVar(void *user_data, const char *key)
    {
        static char tmp[32];
        dmSocket::Address saddr = *((dmSocket::Address*) user_data);
        if (strcmp(key, "HOSTNAME") == 0)
        {
            DM_SNPRINTF(tmp, sizeof(tmp), "%u.%u.%u.%u", (saddr >> 24) & 0xff, (saddr >> 16) & 0xff, (saddr >> 8) & 0xff, (saddr >> 0) & 0xff);
            return tmp;
        }
        return 0;
    }

    static const char* ReplaceHttpHostVar(void *user_data, const char *key)
    {
        SSDP* ssdp = (SSDP*) user_data;
        if (strcmp(key, "HTTP-HOST") == 0)
        {
            return ssdp->m_HttpHost;
        }
        return 0;
    }

    static const char* ReplaceSSDPVar(void* user_data, const char* key)
    {
        SSDP* ssdp = (SSDP*) user_data;

        if (strcmp(key, "HTTPPORT") == 0)
        {
            return ssdp->m_HttpPortText;
        }
        else if (strcmp(key, "MAX_AGE") == 0)
        {
            return ssdp->m_MaxAgeText;
        }
        return 0;
    }

    static const char* ReplaceDeviceVar(void* user_data, const char* key)
    {
        dmSSDP::Device *device = (dmSSDP::Device*) user_data;

        if (strcmp(key, "UDN") == 0)
        {
            return device->m_DeviceDesc->m_UDN;
        }
        else if (strcmp(key, "NT") == 0)
        {
            return device->m_DeviceDesc->m_DeviceType;
        }
        else if (strcmp(key, "DEVICE_TYPE") == 0)
        {
            return device->m_DeviceDesc->m_DeviceType;
        }
        else if (strcmp(key, "ID") == 0)
        {
            return device->m_DeviceDesc->m_Id;
        }

        return 0;
    }

    static const char* ReplaceSearchResponseVar(void* user_data, const char* key)
    {
        SearchResponseContext* context = (SearchResponseContext*) user_data;
        if (strcmp(key, "ST") == 0)
        {
            return context->m_ST;
        }
        return 0;
    }

    static dmSocket::Socket NewSocket()
    {
        dmSocket::Socket socket = dmSocket::INVALID_SOCKET_HANDLE;
        dmSocket::Result sr = dmSocket::New(dmSocket::TYPE_DGRAM, dmSocket::PROTOCOL_UDP, &socket);
        if (sr != dmSocket::RESULT_OK)
            goto bail;

        sr = dmSocket::SetReuseAddress(socket, true);
        if (sr != dmSocket::RESULT_OK)
            goto bail;

/*        sr = dmSocket::SetBroadcast(socket, true);
        if (sr != dmSocket::RESULT_OK)
            goto bail;*/

        return socket;
bail:
        if (socket)
            dmSocket::Delete(socket);

        return dmSocket::INVALID_SOCKET_HANDLE;
    }


    static bool AddressSortPred(dmSocket::IfAddr const &a, dmSocket::IfAddr const &b)
    {
        return a.m_Address < b.m_Address;
    }

    // Internally used only; clean up the LocalAddrSocket entry at index 'slot'
    static void DestroyListeningSocket(SSDP *ssdp, uint32_t slot)
    {
        if (ssdp->m_LocalAddrSocket[slot] != dmSocket::INVALID_SOCKET_HANDLE)
        {
            const uint32_t la = ssdp->m_LocalAddr[slot].m_Address;
            dmLogInfo("SSDP: Done on address %u.%u.%u.%u", (la >> 24) & 0xff, (la >> 16) & 0xff, (la >> 8) & 0xff, (la >> 0) & 0xff);
            dmSocket::Delete(ssdp->m_LocalAddrSocket[slot]);
        }
    }

    // Make sure we have sockets bound to all local if addresses
    static void UpdateListeningSockets(SSDP *ssdp, dmSocket::IfAddr *if_addr, uint32_t if_addr_count)
    {
        dmSocket::Socket new_socket[SSDP_MAX_LOCAL_ADDRESSES];

        // Here, the new list of network addresses is compared with the new list provide with the if_addr* argumenst.
        // Both lists are sorted by address, and the loop compares them side by side to detect
        // 1) Addresses that are no longer present (=> destroy socket)
        // 2) Addresses that are still here (=> keep socket)
        // 3) Addresses that are new (=> new socket)
        uint32_t j=0;
        for (uint32_t i=0;i!=if_addr_count;i++)
        {
            // i = new list idx, j = old list idx
            const dmSocket::Address addr = if_addr[i].m_Address;

            // Skip past all non-matching entries (that must have been destroyed)
            while (j < ssdp->m_LocalAddrCount && ssdp->m_LocalAddr[j].m_Address < addr)
            {
                DestroyListeningSocket(ssdp, j++);
            }

            // If matches address and the socket is valid, keep it. Otherwise make a new.
            if (j < ssdp->m_LocalAddrCount && ssdp->m_LocalAddr[j].m_Address == addr && ssdp->m_LocalAddrSocket[j] != dmSocket::INVALID_SOCKET_HANDLE)
            {
                new_socket[i] = ssdp->m_LocalAddrSocket[j];
                j++;
            }
            else
            {
                new_socket[i] = dmSocket::INVALID_SOCKET_HANDLE;

                dmSocket::Socket s = NewSocket();
                if (s == dmSocket::INVALID_SOCKET_HANDLE)
                    continue;

                if (dmSocket::RESULT_OK != dmSocket::SetMulticastIf(s, addr))
                {
                    dmSocket::Delete(s);
                    continue;
                }

                if (dmSocket::RESULT_OK != dmSocket::Bind(s, addr, 0))
                {
                    dmSocket::Delete(s);
                    continue;
                }

                uint32_t la = addr;
                dmLogInfo("SSDP: Started on address %u.%u.%u.%u", (la >> 24) & 0xff, (la >> 16) & 0xff, (la >> 8) & 0xff, (la >> 0) & 0xff);
                new_socket[i] = s;
            }
        }

        // Cleanup remaining unused
        while (j < ssdp->m_LocalAddrCount)
        {
            DestroyListeningSocket(ssdp, j++);
        }

        // The temporary list of new_socket becomes the new list.
        ssdp->m_LocalAddrCount = if_addr_count;
        memcpy(ssdp->m_LocalAddr, if_addr, sizeof(dmSocket::IfAddr) * if_addr_count);
        memcpy(ssdp->m_LocalAddrSocket, new_socket, sizeof(dmSocket::Socket) * if_addr_count);
    }

    static void HttpHeader(void* user_data, const char* key, const char* value)
    {
        // Catch the 'Host' header value to know through what address the client
        // is connecting using. This same host will be used as replace var to
        // provide a matching address.
        SSDP* ssdp = (SSDP*) user_data;
        if (!dmStrCaseCmp(key, "Host"))
        {
            dmStrlCpy(ssdp->m_HttpHost, value, sizeof(ssdp->m_HttpHost));

            // Strip possible port number included here
            char *delim = strchr(ssdp->m_HttpHost, ':');
            if (delim)
            {
                *delim = 0;
            }
        }
    }

    static void HttpResponse(void* user_data, const dmHttpServer::Request* request)
    {
        SSDP* ssdp = (SSDP*) user_data;

        const char* last_slash = strrchr(request->m_Resource, '/');
        if (!last_slash)
        {
            dmHttpServer::SetStatusCode(request, 400);
            const char* s = "Bad URL";
            dmHttpServer::Send(request, s, strlen(s));
            return;
        }
        const char* id = last_slash + 1;

        dmhash_t id_hash = dmHashString64(id);
        Device** device = ssdp->m_RegistredEntries.Get(id_hash);
        if (!device)
        {
            dmHttpServer::SetStatusCode(request, 404);
            const char* s = "Device not found";
            dmHttpServer::Send(request, s, strlen(s));
            return;
        }

        char buffer[1024];
        Replacer replacer(0, ssdp, ReplaceHttpHostVar);
        dmTemplate::Result tr = dmTemplate::Format(&replacer, buffer, sizeof(buffer), (*device)->m_DeviceDesc->m_DeviceDescription, Replacer::Replace);
        if (tr != dmTemplate::RESULT_OK)
        {
            dmLogError("Error formating http response (%d)", tr);
            const char *s = "Internal error";
            dmHttpServer::Send(request, s, strlen(s));
            return;
        }

        dmHttpServer::Send(request, buffer, strlen(buffer));
    }

    static void Disconnect(SSDP* ssdp)
    {
        if (ssdp->m_MCastSocket != dmSocket::INVALID_SOCKET_HANDLE)
        {
            dmSocket::Delete(ssdp->m_MCastSocket);
            ssdp->m_MCastSocket = dmSocket::INVALID_SOCKET_HANDLE;
        }
    }

    static Result Connect(SSDP* ssdp)
    {
        Disconnect(ssdp);

        dmSocket::Socket mcast_sock = dmSocket::INVALID_SOCKET_HANDLE;
        dmSocket::Result sr;

        mcast_sock = NewSocket();
        if (mcast_sock == dmSocket::INVALID_SOCKET_HANDLE) goto bail;
        sr = dmSocket::Bind(mcast_sock, 0, SSDP_MCAST_PORT);
        if (sr != dmSocket::RESULT_OK) goto bail;
        sr = dmSocket::AddMembership(mcast_sock,
                                     dmSocket::AddressFromIPString(SSDP_MCAST_ADDR),
                                     0,
                                     SSDP_MCAST_TTL);

        if (sr != dmSocket::RESULT_OK)
        {
            dmLogError("Unable to add broadcast membership for ssdp socket. No network connection? (%d)", sr);
        }

        ssdp->m_MCastSocket = mcast_sock;
        return RESULT_OK;

bail:
        if (mcast_sock != dmSocket::INVALID_SOCKET_HANDLE)
            dmSocket::Delete(mcast_sock);

        return RESULT_NETWORK_ERROR;
    }

    Result New(const NewParams* params,  HSSDP* hssdp)
    {
        *hssdp = 0;
        SSDP* ssdp = 0;
        dmHttpServer::HServer http_server = 0;
        dmHttpServer::NewParams http_params;
        dmHttpServer::Result hsr;

        if(params->m_AnnounceInterval > params->m_MaxAge)
        {
            dmLogError("SSDP announceinterval must be less than maxage");
            return RESULT_NETWORK_ERROR;
        }

        ssdp = new SSDP();
        Result r = Connect(ssdp);
        if (r != RESULT_OK) goto bail;

        ssdp->m_MaxAge = params->m_MaxAge;
        DM_SNPRINTF(ssdp->m_MaxAgeText, sizeof(ssdp->m_MaxAgeText), "%u", params->m_MaxAge);
        ssdp->m_Announce = params->m_Announce;
        ssdp->m_AnnounceInterval = params->m_AnnounceInterval;

        *hssdp = ssdp;

        http_params.m_HttpHeader = HttpHeader;
        http_params.m_HttpResponse = HttpResponse;
        http_params.m_Userdata = ssdp;
        hsr = dmHttpServer::New(&http_params, 0, &http_server);
        if (hsr != dmHttpServer::RESULT_OK)
            goto bail;

        ssdp->m_HttpServer = http_server;

        dmSocket::Address http_address;
        uint16_t http_port;
        dmHttpServer::GetName(http_server, &http_address, &http_port);

        DM_SNPRINTF(ssdp->m_HttpPortText, sizeof(ssdp->m_HttpPortText), "%u", http_port);

        return RESULT_OK;

bail:
        Disconnect(ssdp);

        if (http_server)
            dmHttpServer::Delete(http_server);

        delete ssdp;

        return RESULT_NETWORK_ERROR;
    }

    Result Delete(HSSDP ssdp)
    {
        UpdateListeningSockets(ssdp, 0, 0);
        dmHttpServer::Delete(ssdp->m_HttpServer);
        Disconnect(ssdp);
        delete ssdp;
        return RESULT_OK;
    }

    static void SendAnnounce(HSSDP ssdp, Device* device, uint32_t iface)
    {
        assert(iface < ssdp->m_LocalAddrCount);
        dmLogDebug("SSDP Announcing '%s' on interface %s", device->m_DeviceDesc->m_Id, ssdp->m_LocalAddr[iface].m_Name);
        Replacer replacer1(0, device, ReplaceDeviceVar);
        Replacer replacer2(&replacer1, ssdp, ReplaceSSDPVar);
        Replacer replacer3(&replacer2, &ssdp->m_LocalAddr[iface].m_Address, ReplaceIfAddrVar);

        dmTemplate::Result tr = dmTemplate::Format(&replacer3, (char*) ssdp->m_Buffer, sizeof(ssdp->m_Buffer), SSDP_ALIVE_TMPL, Replacer::Replace);
        if (tr != dmTemplate::RESULT_OK)
        {
            dmLogError("Error formating announce message (%d)", tr);
            return;
        }

        int sent_bytes;
        dmSocket::Result sr = dmSocket::SendTo(ssdp->m_LocalAddrSocket[iface], ssdp->m_Buffer, strlen((char*) ssdp->m_Buffer), &sent_bytes, dmSocket::AddressFromIPString(SSDP_MCAST_ADDR), SSDP_MCAST_PORT);
        if (sr != dmSocket::RESULT_OK)
        {
            dmLogWarning("Failed to send announce message (%d)", sr);
        }
    }

    static void SendUnannounce(HSSDP ssdp, Device* device, uint32_t iface)
    {
        Replacer replacer(0, device, ReplaceDeviceVar);
        dmTemplate::Result tr = dmTemplate::Format(&replacer, (char*) ssdp->m_Buffer, sizeof(ssdp->m_Buffer), SSDP_BYEBYE_TMPL, Replacer::Replace);
        if (tr != dmTemplate::RESULT_OK)
        {
            dmLogError("Error formating unannounce message (%d)", tr);
            return;
        }

        int sent_bytes;
        dmSocket::Result sr = dmSocket::SendTo(ssdp->m_LocalAddrSocket[iface], ssdp->m_Buffer, strlen((char*) ssdp->m_Buffer), &sent_bytes, dmSocket::AddressFromIPString(SSDP_MCAST_ADDR), SSDP_MCAST_PORT);
        if (sr != dmSocket::RESULT_OK)
        {
            dmLogWarning("Failed to send unannounce message (%d)", sr);
        }
    }

    Result RegisterDevice(HSSDP ssdp, const DeviceDesc* device_desc)
    {
        const char* id = device_desc->m_Id;
        dmhash_t id_hash = dmHashString64(id);
        if (ssdp->m_RegistredEntries.Get(id_hash) != 0)
        {
            return RESULT_ALREADY_REGISTRED;
        }

        if (ssdp->m_RegistredEntries.Full())
        {
            return RESULT_OUT_OF_RESOURCES;
        }

        Device* device = new Device(device_desc);
        ssdp->m_RegistredEntries.Put(id_hash, device);
        dmLogDebug("SSDP device '%s' registered", id);
        return RESULT_OK;
    }

    Result DeregisterDevice(HSSDP ssdp, const char* id)
    {
        dmhash_t id_hash = dmHashString64(id);
        if (ssdp->m_RegistredEntries.Get(id_hash) == 0)
        {
            return RESULT_NOT_REGISTRED;
        }

        Device** d  = ssdp->m_RegistredEntries.Get(id_hash);

        for (uint32_t i=0;i!=ssdp->m_LocalAddrCount;i++)
        {
            if (ssdp->m_LocalAddrSocket[i] != dmSocket::INVALID_SOCKET_HANDLE)
                SendUnannounce(ssdp, *d, i);
        }
        delete *d;
        ssdp->m_RegistredEntries.Erase(id_hash);
        dmLogDebug("SSDP device '%s' deregistered", id);
        return RESULT_OK;
    }

    void RequestParseState::FreeCallback(RequestParseState *state, const dmhash_t* key, const char** value)
    {
        free((void*) *value);
    }

    static void VersionCallback(void* user_data, int major, int minor, int status, const char* status_str)
    {
        RequestParseState* state = (RequestParseState*) user_data;
        state->m_Status = status;
    }

    static void RequestCallback(void* user_data, const char* request_method, const char* resource, int major, int minor)
    {
        RequestParseState* state = (RequestParseState*) user_data;

        if (strcmp("NOTIFY", request_method) == 0)
            state->m_RequestType = RT_NOTIFY;
        else if (strcmp("M-SEARCH", request_method) == 0)
            state->m_RequestType = RT_M_SEARCH;
        else
            state->m_RequestType = RT_UNKNOWN;
    }

    static void HeaderCallback(void* user_data, const char* orig_key, const char* value)
    {
        RequestParseState* state = (RequestParseState*) user_data;

        char key[64];
        key[sizeof(key)-1] = '\0'; // Ensure NULL termination
        for (uint32_t i = 0; i < sizeof(key); ++i) {
            key[i] = toupper(orig_key[i]);
            if (key[i] == '\0')
                break;
        }

        if (strcmp(key, "CACHE-CONTROL") == 0)
        {
            const char* p= strstr(value, "max-age=");
            if (p)
            {
                state->m_MaxAge = atoi(p + sizeof("max-age=")-1);
            }
        }
        else if (strcmp(key, "NT") == 0)
        {
            state->m_NTHash = dmHashString64(value);
        }
        else if (strcmp(key, "NTS") == 0)
        {
            state->m_NTSHash = dmHashString64(value);
        }

        dmhash_t key_hash = dmHashString64(key);
        state->m_Headers.Put(key_hash, strdup(value));
    }

    static void ResponseCallback(void* user_data, int offset)
    {
        (void) user_data;
        (void) offset;
    }

    static void HandleAnnounce(RequestParseState* state, const char* usn)
    {
        static const dmhash_t location_hash = dmHashString64("LOCATION");

        dmhash_t id = dmHashString64(usn);
        SSDP* ssdp = state->m_SSDP;

        if (ssdp->m_DiscoveredDevices.Get(id) == 0)
        {
            Device device;
            device.m_Expires = dmTime::GetTime() + state->m_MaxAge * uint64_t(1000000);

            // New
            if (ssdp->m_DiscoveredDevices.Full())
            {
                dmLogWarning("Out of SSDP entries. Ignoring message");
                return;
            }
            ssdp->m_DiscoveredDevices.Put(id, device);
            const char* location = "UNKNOWN";
            const char** loc = state->m_Headers.Get(location_hash);
            if (loc)
                location = *loc;
            dmLogDebug("SSDP new %s (%s) (announce/search-response)", usn, location);
        }
        else
        {
            // Renew
            dmLogDebug("SSDP renew %s (announce/search-response)", usn);
            Device* old_device = ssdp->m_DiscoveredDevices.Get(id);
            old_device->m_Expires = dmTime::GetTime() + state->m_MaxAge * uint64_t(1000000);
        }

        if (ssdp->m_DiscoveredDevices.Full())
        {
            dmLogWarning("Out of SSDP entries. Ignoring message");
            return;
        }
    }

    static void HandleUnAnnounce(RequestParseState* state, const char* usn)
    {
        dmhash_t id = dmHashString64(usn);
        SSDP* ssdp = state->m_SSDP;

        if (ssdp->m_DiscoveredDevices.Get(id) != 0)
        {
            dmLogDebug("SSDP unannounce (removing) %s", usn);
            ssdp->m_DiscoveredDevices.Erase(id);
        }
    }

    static void SearchCallback(SearchResponseContext* ctx, const dmhash_t* key, Device** device)
    {
        if (strcmp(ctx->m_ST, (*device)->m_DeviceDesc->m_DeviceType) != 0) {
            return;
        }

        SSDP* ssdp = ctx->m_State->m_SSDP;

        // To know which socket to use for the response, pick the one with the highest
        // number of matching bits. Ideally, netmask would be available to make a correct
        // decision, but it is unfortunately not so easy to read out on all platforms.
        dmSocket::Address local_address = 0;
        dmSocket::Socket output_socket = dmSocket::INVALID_SOCKET_HANDLE;
        uint32_t best_match_value = ~0;
        for (uint32_t i=0;i!=ssdp->m_LocalAddrCount;i++)
        {
            uint32_t non_matching_bits = ssdp->m_LocalAddr[i].m_Address ^ ctx->m_FromAddress;
            if (i == 0 || non_matching_bits < best_match_value)
            {
                best_match_value = non_matching_bits;
                local_address = ssdp->m_LocalAddr[i].m_Address;
                output_socket = ssdp->m_LocalAddrSocket[i];
            }
        }

        if (output_socket == dmSocket::INVALID_SOCKET_HANDLE)
        {
            dmLogError("No output socket available for ssdp search response");
            return;
        }

        dmLogDebug("Sending search response: %s", (*device)->m_DeviceDesc->m_UDN);

        Replacer replacer1(0, *device, ReplaceDeviceVar);
        Replacer replacer2(&replacer1, ctx, ReplaceSearchResponseVar);
        Replacer replacer3(&replacer2, ssdp, ReplaceSSDPVar);
        Replacer replacer4(&replacer3, &local_address, ReplaceIfAddrVar);
        dmTemplate::Result tr = dmTemplate::Format(&replacer4, (char*) ssdp->m_Buffer, sizeof(ssdp->m_Buffer), SEARCH_RESULT_FMT, Replacer::Replace);
        if (tr != dmTemplate::RESULT_OK)
        {
            dmLogError("Error formating search response message (%d)", tr);
            return;
        }

        int sent_bytes;
        dmSocket::SendTo(output_socket, ssdp->m_Buffer, strlen((char*) ssdp->m_Buffer), &sent_bytes, ctx->m_FromAddress, ctx->m_FromPort);
    }

    static void HandleSearch(RequestParseState* state, dmSocket::Address from_address, uint16_t from_port)
    {
        static const dmhash_t st_hash = dmHashString64("ST");
        const char** st = state->m_Headers.Get(st_hash);
        if (!st)
        {
            dmLogWarning("Malformed search package. Missing ST header");
            return;
        }

        SearchResponseContext context(state, *st, from_address, from_port);
        state->m_SSDP->m_RegistredEntries.Iterate(SearchCallback, &context);
    }

    struct ExpireContext
    {
        SSDP*                   m_SSDP;
        uint64_t                m_Now;
        dmArray<dmhash_t>       m_ToExpire;

        ExpireContext(SSDP* ssdp)
        {
            memset(this, 0, sizeof(*this));
            m_SSDP = ssdp;
            m_Now = dmTime::GetTime();
        }
    };

    /**
     * Dispatch socket
     * @param ssdp ssdp handle
     * @param socket socket to dispatch
     * @param response true for response-dispatch
     * @return true on success or on transient errors. false on permanent errors.
     */
    static bool DispatchSocket(SSDP* ssdp, dmSocket::Socket socket, bool response)
    {
        static const dmhash_t usn_hash = dmHashString64("USN");
        static const dmhash_t ssdp_alive_hash = dmHashString64("ssdp:alive");
        static const dmhash_t ssdp_byebye_hash = dmHashString64("ssdp:byebye");

        dmSocket::Result sr;
        int recv_bytes;
        dmSocket::Address from_addr;
        uint16_t from_port;
        sr = dmSocket::ReceiveFrom(socket,
                                   ssdp->m_Buffer,
                                   sizeof(ssdp->m_Buffer),
                                   &recv_bytes, &from_addr, &from_port);

        if (sr != dmSocket::RESULT_OK)
        {
            // When returning from sleep mode on iOS socket is in state ECONNABORTED
            if (sr == dmSocket::RESULT_CONNABORTED || sr == dmSocket::RESULT_NOTCONN)
            {
                dmLogDebug("SSDP permanent dispatch error");
                return false;
            }
            else
            {
                dmLogDebug("SSDP transient dispatch error");
                return true;
            }
        }

        uint8_t comps[4];
        comps[0] = (from_addr >> 24) & 0xff; comps[1] = (from_addr >> 16) & 0xff;
        comps[2] = (from_addr >> 8) & 0xff; comps[3] = (from_addr >> 0) & 0xff;
        dmLogDebug("Multicast SSDP message from %u.%u.%u.%u:%d", comps[0],comps[1],comps[2],comps[3], from_port);

        RequestParseState state(ssdp);
        bool ok = false;

        if (response)
        {
            dmHttpClientPrivate::ParseResult pr = dmHttpClientPrivate::ParseHeader((char*) ssdp->m_Buffer, &state, VersionCallback, HeaderCallback, ResponseCallback);
            ok = pr == dmHttpClientPrivate::PARSE_RESULT_OK;
        }
        else
        {
            dmHttpServerPrivate::ParseResult pr = dmHttpServerPrivate::ParseHeader((char*) ssdp->m_Buffer, &state, RequestCallback, HeaderCallback, ResponseCallback);
            ok = pr == dmHttpServerPrivate::PARSE_RESULT_OK;
        }

        if (ok)
        {
            const char** usn = state.m_Headers.Get(usn_hash);

            if (response)
            {
                if (state.m_Status == 200)
                {
                    if (usn != 0)
                    {
                        HandleAnnounce(&state, *usn);
                    }
                    else
                    {
                        dmLogWarning("Malformed message from %u.%u.%u.%u:%d. Missing USN header.", comps[0],comps[1],comps[2],comps[3], from_port);
                    }
                }
            }
            else
            {
                if (state.m_RequestType == RT_NOTIFY)
                {
                    if (usn != 0)
                    {
                        if (state.m_NTSHash == ssdp_alive_hash)
                        {
                            HandleAnnounce(&state, *usn);
                        }
                        else if (state.m_NTSHash == ssdp_byebye_hash)
                        {
                            HandleUnAnnounce(&state, *usn);
                        }
                    }
                    else
                    {
                        dmLogWarning("Malformed message from %u.%u.%u.%u:%d. Missing USN header.", comps[0],comps[1],comps[2],comps[3], from_port);
                    }
                }
                else if (state.m_RequestType == RT_M_SEARCH)
                {
                    HandleSearch(&state, from_addr, from_port);
                }
            }
        }
        else
        {
            dmLogWarning("Malformed message from %u.%u.%u.%u:%d", comps[0],comps[1],comps[2],comps[3], from_port);
        }

       return true;
    }

    static void VisitDiscoveredExpireDevice(ExpireContext* context, const dmhash_t* key, Device* device)
    {
        if (context->m_Now >= device->m_Expires)
        {
            if (context->m_ToExpire.Full())
            {
                context->m_ToExpire.OffsetCapacity(64);
            }
            context->m_ToExpire.Push(*key);
        }
    }

    static void ExpireDiscovered(SSDP* ssdp)
    {
        ExpireContext context(ssdp);
        ssdp->m_DiscoveredDevices.Iterate(VisitDiscoveredExpireDevice, &context);
        uint32_t n = context.m_ToExpire.Size();
        for(uint32_t i = 0; i < n; ++i)
        {
            uint64_t id = context.m_ToExpire[i];
            dmLogDebug("SSDP expired %s", (const char*) dmHashReverse64(id, 0));

            ssdp->m_DiscoveredDevices.Erase(id);
        }
    }

    static void VisitRegistredAnnounceDevice(SSDP* ssdp, const dmhash_t* key, Device** device)
    {
        const uint64_t now = dmTime::GetTime();
        const uint64_t next = now + ssdp->m_AnnounceInterval * uint64_t(1000000);
        Device *dev = *device;

        // Each device keeps a list of the interface address states; address and timeout. It is a sorted
        // list and so is m_LocalAddrs, so side-by-side comparison allows easy matching with the sockes
        // and addresses in m_LocalAddr.
        uint64_t new_expires[SSDP_MAX_LOCAL_ADDRESSES];
        for (uint32_t i=0,j=0;i!=ssdp->m_LocalAddrCount;i++)
        {
            while (j < dev->m_IfAddrStateCount && dev->m_IfAddrState[j].m_Address < ssdp->m_LocalAddr[i].m_Address)
                ++j;

            if (j < dev->m_IfAddrStateCount && dev->m_IfAddrState[j].m_Address == ssdp->m_LocalAddr[i].m_Address)
            {
                // Found previous expiry time for this interface
                new_expires[i] = dev->m_IfAddrState[j++].m_Expires;
            }
            else
            {
                // This address has no expiry time (new interface), send right away
                new_expires[i] = now;
            }
        }

        // Send announces on all interfaces.
        dev->m_IfAddrStateCount = ssdp->m_LocalAddrCount;
        for (uint32_t i=0;i!=ssdp->m_LocalAddrCount;i++)
        {
            Device::IfAddrState *dst = &dev->m_IfAddrState[i];
            dst->m_Address = ssdp->m_LocalAddr[i].m_Address;
            if (new_expires[i] <= now)
            {
                SendAnnounce(ssdp, dev, i);
                dst->m_Expires = next;
            }
            else
            {
                dst->m_Expires = new_expires[i];
            }
        }
    }

    static void AnnounceRegistred(SSDP* ssdp)
    {
        ssdp->m_RegistredEntries.Iterate(VisitRegistredAnnounceDevice, ssdp);
    }

    void Update(HSSDP ssdp, bool search)
    {
        if (ssdp->m_Reconnect)
        {
            dmLogWarning("Reconnecting SSDP");
            Connect(ssdp);
            ssdp->m_Reconnect = 0;
        }

        const uint64_t now = dmTime::GetTime();
        if (now > ssdp->m_LocalAddrExpires)
        {
            // Grab a sorted list of network addresses, with all the wildcard addresses (0) removed
            // and make sure there are sockets for each of these
            ssdp->m_LocalAddrExpires = now + SSDP_LOCAL_ADDRESS_EXPIRATION * uint64_t(1000000);

            uint32_t if_addr_count;
            dmSocket::IfAddr if_addr[SSDP_MAX_LOCAL_ADDRESSES];
            dmSocket::GetIfAddresses(if_addr, SSDP_MAX_LOCAL_ADDRESSES, &if_addr_count);
            std::sort(if_addr, if_addr + if_addr_count, AddressSortPred);

            // Remove zeroes which are all at the starts
            dmSocket::IfAddr *begin = &if_addr[0];
            dmSocket::IfAddr *end = begin + if_addr_count;
            while (begin < end && !begin->m_Address)
                ++begin;

            UpdateListeningSockets(ssdp, begin, end - begin);
        }

        ExpireDiscovered(ssdp);

        if (ssdp->m_Announce)
        {
            AnnounceRegistred(ssdp);
        }

        dmHttpServer::Update(ssdp->m_HttpServer);

        bool incoming_data;
        do
        {
            incoming_data = false;
            dmSocket::Selector selector;
            dmSocket::SelectorZero(&selector);
            dmSocket::SelectorSet(&selector, dmSocket::SELECTOR_KIND_READ, ssdp->m_MCastSocket);
            for (uint32_t i=0;i<ssdp->m_LocalAddrCount;i++)
            {
                if (ssdp->m_LocalAddrSocket[i] != dmSocket::INVALID_SOCKET_HANDLE)
                    dmSocket::SelectorSet(&selector, dmSocket::SELECTOR_KIND_READ, ssdp->m_LocalAddrSocket[i]);
            }
            dmSocket::Select(&selector, 0);

            if (dmSocket::SelectorIsSet(&selector, dmSocket::SELECTOR_KIND_READ, ssdp->m_MCastSocket))
            {
                if (DispatchSocket(ssdp, ssdp->m_MCastSocket, false))
                {
                    incoming_data = true;
                }
                else
                {
                    ssdp->m_Reconnect = 1;
                }
            }

            for (uint32_t i=0;i<ssdp->m_LocalAddrCount;i++)
            {
                if (ssdp->m_LocalAddrSocket[i] == dmSocket::INVALID_SOCKET_HANDLE)
                    continue;

                if (dmSocket::SelectorIsSet(&selector, dmSocket::SELECTOR_KIND_READ, ssdp->m_LocalAddrSocket[i]))
                {
                    if (DispatchSocket(ssdp, ssdp->m_LocalAddrSocket[i], true))
                        incoming_data = true;
                }
            }
        } while (incoming_data);

        if (search)
        {
            for (uint32_t i=0;i!=ssdp->m_LocalAddrCount;i++)
            {
                if (ssdp->m_LocalAddrSocket[i] == dmSocket::INVALID_SOCKET_HANDLE)
                    continue;

                int sent_bytes;
                dmSocket::Result sr;
                sr = dmSocket::SendTo(ssdp->m_LocalAddrSocket[i],
                                 M_SEARCH_FMT,
                                 strlen(M_SEARCH_FMT),
                                 &sent_bytes,
                                 dmSocket::AddressFromIPString(SSDP_MCAST_ADDR),
                                 SSDP_MCAST_PORT);

                dmLogDebug("SSDP M-SEARCH");
                if (sr != dmSocket::RESULT_OK)
                {
                    dmLogWarning("Failed to send SSDP search package (%d)", sr);
                }
            }
        }
    }

    void ClearDiscovered(HSSDP ssdp)
    {
        ssdp->m_DiscoveredDevices.Clear();
    }

    void IterateDevicesInternal(HSSDP ssdp, void (*call_back)(void *context, const dmhash_t* usn, Device* device), void* context)
    {
        assert(ssdp);
        ssdp->m_DiscoveredDevices.Iterate(call_back, context);
    }

}  // namespace dmSSDP


