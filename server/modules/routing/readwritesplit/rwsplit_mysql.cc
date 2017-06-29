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

#include "readwritesplit.hh"
#include "rwsplit_internal.hh"

#include <strings.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#include <maxscale/router.h>
#include <maxscale/log_manager.h>
#include <maxscale/query_classifier.h>
#include <maxscale/dcb.h>
#include <maxscale/spinlock.h>
#include <maxscale/modinfo.h>
#include <maxscale/modutil.h>
#include <maxscale/protocol/mysql.h>
#include <maxscale/alloc.h>

#define RWSPLIT_TRACE_MSG_LEN 1000

/**
 * Functions within the read-write split router that are specific to
 * MySQL. The aim is to either remove these into a separate module or to
 * move them into the MySQL protocol modules.
 */

/*
 * The following functions are called from elsewhere in the router and
 * are defined in rwsplit_internal.hh.  They are not intended to be called
 * from outside this router.
 */

/* This could be placed in the protocol, with a new API entry point
 * It is certainly MySQL specific. Packet types are DB specific, but can be
 * assumed to be enums, which can be handled as integers without knowing
 * which DB is involved until the packet type needs to be interpreted.
 *
 */

/**
 * @brief Determine packet type
 *
 * Examine the packet in the buffer to extract the type, if possible. At the
 * same time set the second parameter to indicate whether the packet was
 * empty.
 *
 * It is assumed that the packet length and type are contained within a single
 * buffer, the one indicated by the first parameter.
 *
 * @param querybuf  Buffer containing the packet
 * @param non_empty_packet  bool indicating whether the packet is non-empty
 * @return The packet type, or MYSQL_COM_UNDEFINED; also the second parameter is set
 */
uint8_t
determine_packet_type(GWBUF *querybuf, bool *non_empty_packet)
{
    uint8_t packet_type;
    uint8_t *packet = GWBUF_DATA(querybuf);

    if (gw_mysql_get_byte3(packet) == 0)
    {
        /** Empty packet signals end of LOAD DATA LOCAL INFILE, send it to master*/
        *non_empty_packet = false;
        packet_type = (uint8_t)MYSQL_COM_UNDEFINED;
    }
    else
    {
        *non_empty_packet = true;
        packet_type = packet[4];
    }
    return packet_type;
}

/*
 * This appears to be MySQL specific
 */
/**
 * @brief Determine if a packet contains a SQL query
 *
 * Packet type tells us this, but in a DB specific way. This function is
 * provided so that code that is not DB specific can find out whether a packet
 * contains a SQL query. Clearly, to be effective different functions must be
 * called for different DB types.
 *
 * @param packet_type   Type of packet (integer)
 * @return bool indicating whether packet contains a SQL query
 */
bool
is_packet_a_query(int packet_type)
{
    return (packet_type == MYSQL_COM_QUERY);
}

/*
 * This looks MySQL specific
 */
/**
 * @brief Determine if a packet contains a one way message
 *
 * Packet type tells us this, but in a DB specific way. This function is
 * provided so that code that is not DB specific can find out whether a packet
 * contains a one way messsage. Clearly, to be effective different functions must be
 * called for different DB types.
 *
 * @param packet_type   Type of packet (integer)
 * @return bool indicating whether packet contains a one way message
 */
bool command_will_respond(uint8_t packet_type)
{
    return packet_type != MYSQL_COM_STMT_SEND_LONG_DATA &&
           packet_type != MYSQL_COM_QUIT &&
           packet_type != MYSQL_COM_STMT_CLOSE;
}

/*
 * This one is problematic because it is MySQL specific, but also router
 * specific.
 */
/**
 * @brief Log the transaction status
 *
 * The router session and the query buffer are used to log the transaction
 * status, along with the query type (which is a generic description that
 * should be usable across all DB types).
 *
 * @param rses      Router session
 * @param querybuf  Query buffer
 * @param qtype     Query type
 */
void
log_transaction_status(ROUTER_CLIENT_SES *rses, GWBUF *querybuf, uint32_t qtype)
{
    if (rses->load_data_state == LOAD_DATA_INACTIVE)
    {
        uint8_t *packet = GWBUF_DATA(querybuf);
        unsigned char command = packet[4];
        int len = 0;
        char* sql;
        modutil_extract_SQL(querybuf, &sql, &len);
        char *qtypestr = qc_typemask_to_string(qtype);

        if (len > RWSPLIT_TRACE_MSG_LEN)
        {
            len = RWSPLIT_TRACE_MSG_LEN;
        }

        MXS_SESSION *ses = rses->client_dcb->session;
        const char *autocommit = session_is_autocommit(ses) ? "[enabled]" : "[disabled]";
        const char *transaction = session_trx_is_active(ses) ? "[open]" : "[not open]";
        const char *querytype = qtypestr == NULL ? "N/A" : qtypestr;
        const char *hint = querybuf->hint == NULL ? "" : ", Hint:";
        const char *hint_type = querybuf->hint == NULL ? "" : STRHINTTYPE(querybuf->hint->type);

        MXS_INFO("> Autocommit: %s, trx is %s, cmd: (0x%02x) %s, type: %s, stmt: %.*s%s %s",
                 autocommit, transaction, command, STRPACKETTYPE(command),
                 querytype, len, sql, hint, hint_type);

        MXS_FREE(qtypestr);
    }
    else
    {
        MXS_INFO("> Processing LOAD DATA LOCAL INFILE: %lu bytes sent.",
                 rses->rses_load_data_sent);
    }
}

/*
 * This is mostly router code, but it contains MySQL specific operations that
 * maybe could be moved to the protocol module. The modutil functions are mostly
 * MySQL specific and could migrate to the MySQL protocol; likewise the
 * utility to convert packet type to a string. The aim is for most of this
 * code to remain as part of the router.
 */
/**
 * @brief Operations to be carried out if request is for all backend servers
 *
 * If the choice of sending to all backends is in conflict with other bit
 * settings in route_target, then error messages are written to the log.
 *
 * Otherwise, the function route_session_write is called to carry out the
 * actual routing.
 *
 * @param route_target  Bit map indicating where packet should be routed
 * @param inst          Router instance
 * @param rses          Router session
 * @param querybuf      Query buffer containing packet
 * @param packet_type   Integer (enum) indicating type of packet
 * @param qtype         Query type
 * @return bool indicating whether the session can continue
 */
bool handle_target_is_all(route_target_t route_target, ROUTER_INSTANCE *inst,
                          ROUTER_CLIENT_SES *rses, GWBUF *querybuf,
                          int packet_type, uint32_t qtype)
{
    bool result = false;

    if (TARGET_IS_MASTER(route_target) || TARGET_IS_SLAVE(route_target))
    {
        /**
         * Conflicting routing targets. Return an error to the client.
         */

        char *query_str = modutil_get_query(querybuf);
        char *qtype_str = qc_typemask_to_string(qtype);

        MXS_ERROR("Can't route %s:%s:\"%s\". SELECT with session data "
                  "modification is not supported if configuration parameter "
                  "use_sql_variables_in=all .", STRPACKETTYPE(packet_type),
                  qtype_str, (query_str == NULL ? "(empty)" : query_str));

        GWBUF *errbuf = modutil_create_mysql_err_msg(1, 0, 1064, "42000",
                                                     "Routing query to backend failed. "
                                                     "See the error log for further details.");

        if (errbuf)
        {
            rses->client_dcb->func.write(rses->client_dcb, errbuf);
            result = true;
        }

        MXS_FREE(query_str);
        MXS_FREE(qtype_str);
    }
    else if (route_session_write(rses, gwbuf_clone(querybuf), packet_type, qtype))
    {

        result = true;
        atomic_add_uint64(&inst->stats.n_all, 1);
    }

    return result;
}

/*
 * Probably MySQL specific because of modutil function
 */
/**
 * @brief Write an error message to the log for closed session
 *
 * This happens if a request is received for a session that is already
 * closing down.
 *
 * @param querybuf      Query buffer containing packet
 */
void closed_session_reply(GWBUF *querybuf)
{
    uint8_t* data = GWBUF_DATA(querybuf);

    if (GWBUF_LENGTH(querybuf) >= 5 && !MYSQL_IS_COM_QUIT(data))
    {
        /* Note that most modutil functions are MySQL specific */
        char *query_str = modutil_get_query(querybuf);
        MXS_ERROR("Can't route %s:\"%s\" to backend server. Router is closed.",
                  STRPACKETTYPE(data[4]), query_str ? query_str : "(empty)");
        MXS_FREE(query_str);
    }
}

/*
 * Uses MySQL specific mechanisms
 */
/**
 * @brief Check the reply from a backend server to a session command
 *
 * If the reply is an error, a message is logged.
 *
 * @param buffer  Query buffer containing reply data
 * @param backend Router session data for a backend server
 */
void check_session_command_reply(GWBUF *buffer, SRWBackend backend)
{
    if (MYSQL_IS_ERROR_PACKET(((uint8_t *)GWBUF_DATA(buffer))))
    {
        size_t replylen = MYSQL_GET_PAYLOAD_LEN(GWBUF_DATA(buffer));
        char replybuf[replylen];
        gwbuf_copy_data(buffer, 0, gwbuf_length(buffer), (uint8_t*)replybuf);
        std::string err;
        std::string msg;
        err.append(replybuf + 8, 5);
        msg.append(replybuf + 13, replylen - 4 - 5);

        MXS_ERROR("Failed to execute session command in %s. Error was: %s %s",
                  backend->uri(), err.c_str(), msg.c_str());
    }
}

/**
 * @brief Send an error message to the client telling that the server is in read only mode
 *
 * @param dcb Client DCB
 *
 * @return True if sending the message was successful, false if an error occurred
 */
bool send_readonly_error(DCB *dcb)
{
    bool succp = false;
    const char* errmsg = "The MariaDB server is running with the --read-only"
                         " option so it cannot execute this statement";
    GWBUF* err = modutil_create_mysql_err_msg(1, 0, ER_OPTION_PREVENTS_STATEMENT,
                                              "HY000", errmsg);

    if (err)
    {
        succp = dcb->func.write(dcb, err);
    }
    else
    {
        MXS_ERROR("Memory allocation failed when creating client error message.");
    }

    return succp;
}
