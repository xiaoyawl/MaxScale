#pragma once
/*
 * Copyright (c) 2016 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2020-01-01
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

#include "schemarouter.hh"

#include <string>
#include <list>

#include <maxscale/protocol/mysql.h>
#include <maxscale/router.hh>

#include "shard_map.hh"
#include "session_command.hh"

using std::string;
using std::list;

using namespace schemarouter;

/**
 * Bitmask values for the router session's initialization. These values are used
 * to prevent responses from internal commands being forwarded to the client.
 */
enum init_mask
{
    INIT_READY   = 0x00,
    INIT_MAPPING = 0x01,
    INIT_USE_DB  = 0x02,
    INIT_UNINT   = 0x04,
    INIT_FAILED  = 0x08
};

enum showdb_response
{
    SHOWDB_FULL_RESPONSE,
    SHOWDB_PARTIAL_RESPONSE,
    SHOWDB_DUPLICATE_DATABASES,
    SHOWDB_FATAL_ERROR
};

#define SCHEMA_ERR_DUPLICATEDB 5000
#define SCHEMA_ERRSTR_DUPLICATEDB "DUPDB"
#define SCHEMA_ERR_DBNOTFOUND 1049
#define SCHEMA_ERRSTR_DBNOTFOUND "42000"

/**
 * Route target types
 */
enum route_target
{
    TARGET_UNDEFINED,
    TARGET_NAMED_SERVER,
    TARGET_ALL,
    TARGET_ANY
};

/** Helper macros for route target type */
#define TARGET_IS_UNDEFINED(t)    (t == TARGET_UNDEFINED)
#define TARGET_IS_NAMED_SERVER(t) (t == TARGET_NAMED_SERVER)
#define TARGET_IS_ALL(t)          (t == TARGET_ALL)
#define TARGET_IS_ANY(t)          (t == TARGET_ANY)

class SchemaRouter;

/**
 * The client session structure used within this router.
 */
class SchemaRouterSession: public mxs::RouterSession
{
public:

    SchemaRouterSession(MXS_SESSION* session, SchemaRouter* router, BackendList& backends);

    /**
     * The RouterSession instance will be deleted when a client session
     * has terminated. Will be called only after @c close() has been called.
     */
    ~SchemaRouterSession();

    /**
     * Called when a client session has been closed.
     */
    void close();

    /**
     * Called when a packet being is routed to the backend. The router should
     * forward the packet to the appropriate server(s).
     *
     * @param pPacket A client packet.
     */
    int32_t routeQuery(GWBUF* pPacket);

    /**
     * Called when a packet is routed to the client. The router should
     * forward the packet to the client using `MXS_SESSION_ROUTE_REPLY`.
     *
     * @param pPacket  A client packet.
     * @param pBackend The backend the packet is coming from.
     */
    void clientReply(GWBUF* pPacket, DCB* pBackend);

    /**
     *
     * @param pMessage  The rror message.
     * @param pProblem  The DCB on which the error occurred.
     * @param action    The context.
     * @param pSuccess  On output, if false, the session will be terminated.
     */
    void handleError(GWBUF*             pMessage,
                     DCB*               pProblem,
                     mxs_error_action_t action,
                     bool*              pSuccess);
private:
    /**
     * Internal functions
     */

    /** Helper functions */
    SERVER*  get_shard_target(GWBUF* buffer, uint32_t qtype);
    SBackend get_bref_from_dcb(DCB* dcb);
    bool     get_shard_dcb(DCB** dcb, char* name);
    bool     have_servers();
    bool     handle_default_db();
    bool     ignore_duplicate_database(const char* data);

    /** Routing functions */
    bool    route_session_write(GWBUF* querybuf, uint8_t command);
    void    process_sescmd_response(SBackend& bref, GWBUF** ppPacket);
    SERVER* resolve_query_target(GWBUF* pPacket, uint32_t type, uint8_t command,
                                 enum route_target& route_target);

    /** Shard mapping functions */
    bool                 send_databases();
    bool                 send_shards();
    void                 query_databases();
    int                  inspect_mapping_states(SBackend& bref, GWBUF** wbuf);
    enum showdb_response parse_mapping_response(SBackend& bref, GWBUF** buffer);
    void                 route_queued_query();
    void                 synchronize_shards();
    void                 handle_mapping_reply(SBackend& bref, GWBUF** pPacket);

    /** Member variables */
    bool                  m_closed;         /**< True if session closed */
    DCB*                  m_client;         /**< The client DCB */
    MYSQL_session*        m_mysql_session;  /**< Session client data (username, password, SHA1). */
    BackendList           m_backends;       /**< Backend references */
    Config*               m_config;         /**< Pointer to router config */
    SchemaRouter*         m_router;         /**< The router instance */
    Shard                 m_shard;          /**< Database to server mapping */
    string                m_connect_db;     /**< Database the user was trying to connect to */
    string                m_current_db;     /**< Current active database */
    int                   m_state;          /**< Initialization state bitmask */
    list<Buffer>          m_queue;          /**< Query that was received before the session was ready */
    Stats                 m_stats;          /**< Statistics for this router */
    uint64_t              m_sent_sescmd;    /**< The latest session command being executed */
    uint64_t              m_replied_sescmd; /**< The last session command reply that was sent to the client */
    SERVER*               m_load_target;    /**< Target for LOAD DATA LOCAL INFILE */
};