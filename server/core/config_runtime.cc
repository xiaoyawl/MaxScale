/*
 * Copyright (c) 2016 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2019-07-01
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

#include <maxscale/cppdefs.hh>

#include "maxscale/config_runtime.h"

#include <strings.h>
#include <string>
#include <sstream>
#include <set>
#include <iterator>
#include <algorithm>

#include <maxscale/atomic.h>
#include <maxscale/paths.h>
#include <maxscale/platform.h>
#include <maxscale/spinlock.h>
#include <maxscale/jansson.hh>
#include <maxscale/json_api.h>

#include "maxscale/config.h"
#include "maxscale/monitor.h"
#include "maxscale/modules.h"
#include "maxscale/service.h"

using std::string;
using std::stringstream;
using std::set;
using mxs::Closer;

static SPINLOCK crt_lock = SPINLOCK_INIT;

#define RUNTIME_ERRMSG_BUFSIZE 512
thread_local char runtime_errmsg[RUNTIME_ERRMSG_BUFSIZE];

static void runtime_error(const char* fmt, ...)
{
    va_list list;
    va_start(list, fmt);
    vsnprintf(runtime_errmsg, sizeof(runtime_errmsg), fmt, list);
    va_end(list);
}

static string runtime_get_error()
{
    string rval(runtime_errmsg);
    runtime_errmsg[0] = '\0';
    return rval;
}

bool runtime_link_server(SERVER *server, const char *target)
{
    spinlock_acquire(&crt_lock);

    bool rval = false;
    SERVICE *service = service_find(target);
    MXS_MONITOR *monitor = service ? NULL : monitor_find(target);

    if (service)
    {
        if (serviceAddBackend(service, server))
        {
            service_serialize(service);
            rval = true;
        }
        else
        {
            runtime_error("Service '%s' already uses server '%s'",
                          service->name, server->unique_name);
        }
    }
    else if (monitor)
    {
        if (monitorAddServer(monitor, server))
        {
            monitor_serialize(monitor);
            rval = true;
        }
        else
        {
            runtime_error("Server '%s' is already monitored", server->unique_name);
        }
    }

    if (rval)
    {
        const char *type = service ? "service" : "monitor";
        MXS_NOTICE("Added server '%s' to %s '%s'", server->unique_name, type, target);
    }

    spinlock_release(&crt_lock);
    return rval;
}

bool runtime_unlink_server(SERVER *server, const char *target)
{
    spinlock_acquire(&crt_lock);

    bool rval = false;
    SERVICE *service = service_find(target);
    MXS_MONITOR *monitor = service ? NULL : monitor_find(target);

    if (service || monitor)
    {
        rval = true;

        if (service)
        {
            serviceRemoveBackend(service, server);
            service_serialize(service);
        }
        else if (monitor)
        {
            monitorRemoveServer(monitor, server);
            monitor_serialize(monitor);
        }

        const char *type = service ? "service" : "monitor";
        MXS_NOTICE("Removed server '%s' from %s '%s'", server->unique_name, type, target);
    }

    spinlock_release(&crt_lock);
    return rval;
}

bool runtime_create_server(const char *name, const char *address, const char *port,
                           const char *protocol, const char *authenticator,
                           const char *authenticator_options)
{
    spinlock_acquire(&crt_lock);
    bool rval = false;

    if (server_find_by_unique_name(name) == NULL)
    {
        // TODO: Get default values from the protocol module
        if (port == NULL)
        {
            port = "3306";
        }
        if (protocol == NULL)
        {
            protocol = "MySQLBackend";
        }
        if (authenticator == NULL && (authenticator = get_default_authenticator(protocol)) == NULL)
        {
            MXS_ERROR("No authenticator defined for server '%s' and no default "
                      "authenticator for protocol '%s'.", name, protocol);
            spinlock_release(&crt_lock);
            return false;
        }

        /** First check if this service has been created before */
        SERVER *server = server_find_destroyed(name, protocol, authenticator,
                                               authenticator_options);

        if (server)
        {
            /** Found old server, replace network details with new ones and
             * reactivate it */
            snprintf(server->name, sizeof(server->name), "%s", address);
            server->port = atoi(port);
            server->is_active = true;
            rval = true;
        }
        else
        {
            /**
             * server_alloc will add the server to the global list of
             * servers so we don't need to manually add it.
             */
            server = server_alloc(name, address, atoi(port), protocol,
                                  authenticator, authenticator_options);
        }

        if (server && server_serialize(server))
        {
            /** Mark that the server was created after startup */
            server->created_online = true;

            MXS_NOTICE("Created server '%s' at %s:%u", server->unique_name,
                       server->name, server->port);
            rval = true;
        }
    }
    else
    {
        runtime_error("Server '%s' already exists", name);
    }

    spinlock_release(&crt_lock);
    return rval;
}

bool runtime_destroy_server(SERVER *server)
{
    spinlock_acquire(&crt_lock);
    bool rval = false;

    if (service_server_in_use(server) || monitor_server_in_use(server))
    {
        const char* err = "Cannot destroy server '%s' as it is used by at least "
                          "one service or monitor";
        runtime_error(err, server->unique_name);
        MXS_ERROR(err, server->unique_name);
    }
    else
    {
        char filename[PATH_MAX];
        snprintf(filename, sizeof(filename), "%s/%s.cnf", get_config_persistdir(),
                 server->unique_name);

        if (unlink(filename) == -1)
        {
            if (errno != ENOENT)
            {
                MXS_ERROR("Failed to remove persisted server configuration '%s': %d, %s",
                          filename, errno, mxs_strerror(errno));
            }
            else
            {
                rval = true;
                MXS_WARNING("Server '%s' was not created at runtime. Remove the "
                            "server manually from the correct configuration file.",
                            server->unique_name);
            }
        }
        else
        {
            rval = true;
        }

        if (rval)
        {
            MXS_NOTICE("Destroyed server '%s' at %s:%u", server->unique_name,
                       server->name, server->port);
            server->is_active = false;
        }
    }

    spinlock_release(&crt_lock);
    return rval;
}

static SSL_LISTENER* create_ssl(const char *name, const char *key, const char *cert,
                                const char *ca, const char *version, const char *depth)
{
    SSL_LISTENER *rval = NULL;
    CONFIG_CONTEXT *obj = config_context_create(name);

    if (obj)
    {
        if (config_add_param(obj, CN_SSL, CN_REQUIRED) &&
            config_add_param(obj, CN_SSL_KEY, key) &&
            config_add_param(obj, CN_SSL_CERT, cert) &&
            config_add_param(obj, CN_SSL_CA_CERT, ca) &&
            (!version || config_add_param(obj, CN_SSL_VERSION, version)) &&
            (!depth || config_add_param(obj, CN_SSL_CERT_VERIFY_DEPTH, depth)))
        {
            int err = 0;
            SSL_LISTENER *ssl = make_ssl_structure(obj, true, &err);

            if (err == 0 && ssl && listener_init_SSL(ssl) == 0)
            {
                rval = ssl;
            }
        }

        config_context_free(obj);
    }

    return rval;
}

bool runtime_enable_server_ssl(SERVER *server, const char *key, const char *cert,
                               const char *ca, const char *version, const char *depth)
{
    bool rval = false;

    if (key && cert && ca)
    {
        spinlock_acquire(&crt_lock);
        SSL_LISTENER *ssl = create_ssl(server->unique_name, key, cert, ca, version, depth);

        if (ssl)
        {
            /** TODO: Properly discard old SSL configurations.This could cause the
             * loss of a pointer if two update operations are done at the same time.*/
            ssl->next = server->server_ssl;

            /** Sync to prevent reads on partially initialized server_ssl */
            atomic_synchronize();
            server->server_ssl = ssl;

            if (server_serialize(server))
            {
                MXS_NOTICE("Enabled SSL for server '%s'", server->unique_name);
                rval = true;
            }
        }
        spinlock_release(&crt_lock);
    }

    return rval;
}

bool runtime_alter_server(SERVER *server, const char *key, const char *value)
{
    spinlock_acquire(&crt_lock);
    bool valid = true;

    if (strcmp(key, CN_ADDRESS) == 0)
    {
        server_update_address(server, value);
    }
    else if (strcmp(key, CN_PORT) == 0)
    {
        server_update_port(server, atoi(value));
    }
    else if (strcmp(key, CN_MONITORUSER) == 0)
    {
        server_update_credentials(server, value, server->monpw);
    }
    else if (strcmp(key, CN_MONITORPW) == 0)
    {
        server_update_credentials(server, server->monuser, value);
    }
    else
    {
        if (!server_remove_parameter(server, key) && !value[0])
        {
            valid = false;
        }
        else if (value[0])
        {
            server_add_parameter(server, key, value);

            /**
             * It's likely that this parameter is used as a weighting parameter.
             * We need to update the weights of services that use this.
             */
            service_update_weights();
        }
    }

    if (valid)
    {
        if (server_serialize(server))
        {
            MXS_NOTICE("Updated server '%s': %s=%s", server->unique_name, key, value);
        }
    }
    else
    {
        runtime_error("Invalid server parameter: %s", key);
    }

    spinlock_release(&crt_lock);
    return valid;
}

/**
 * @brief Convert a string value to a positive integer
 *
 * If the value is not a positive integer, an error is printed to @c dcb.
 *
 * @param value String value
 * @return 0 on error, otherwise a positive integer
 */
static long get_positive_int(const char *value)
{
    char *endptr;
    long ival = strtol(value, &endptr, 10);

    if (*endptr == '\0' && ival > 0)
    {
        return ival;
    }

    return 0;
}

/**
 * @brief Add default parameters to a monitor
 *
 * @param monitor Monitor to modify
 */
static void add_monitor_defaults(MXS_MONITOR *monitor)
{
    /** Inject the default module parameters in case we only deleted
     * a parameter */
    CONFIG_CONTEXT ctx = {};
    ctx.object = (char*)"";
    const MXS_MODULE *mod = get_module(monitor->module_name, MODULE_MONITOR);

    if (mod)
    {
        config_add_defaults(&ctx, mod->parameters);
        monitorAddParameters(monitor, ctx.parameters);
        config_parameter_free(ctx.parameters);
    }
    else
    {
        MXS_ERROR("Failed to load module '%s'. See previous error messages for more details.",
                  monitor->module_name);
    }
}

bool runtime_alter_monitor(MXS_MONITOR *monitor, const char *key, const char *value)
{
    spinlock_acquire(&crt_lock);
    bool valid = false;

    if (strcmp(key, CN_USER) == 0)
    {
        valid = true;
        monitorAddUser(monitor, value, monitor->password);
    }
    else if (strcmp(key, CN_PASSWORD) == 0)
    {
        valid = true;
        monitorAddUser(monitor, monitor->user, value);
    }
    else if (strcmp(key, CN_MONITOR_INTERVAL) == 0)
    {
        long ival = get_positive_int(value);
        if (ival)
        {
            valid = true;
            monitorSetInterval(monitor, ival);
        }
    }
    else if (strcmp(key, CN_BACKEND_CONNECT_TIMEOUT) == 0)
    {
        long ival = get_positive_int(value);
        if (ival)
        {
            valid = true;
            monitorSetNetworkTimeout(monitor, MONITOR_CONNECT_TIMEOUT, ival);
        }
    }
    else if (strcmp(key, CN_BACKEND_WRITE_TIMEOUT) == 0)
    {
        long ival = get_positive_int(value);
        if (ival)
        {
            valid = true;
            monitorSetNetworkTimeout(monitor, MONITOR_WRITE_TIMEOUT, ival);
        }
    }
    else if (strcmp(key, CN_BACKEND_READ_TIMEOUT) == 0)
    {
        long ival = get_positive_int(value);
        if (ival)
        {
            valid = true;
            monitorSetNetworkTimeout(monitor, MONITOR_READ_TIMEOUT, ival);
        }
    }
    else if (strcmp(key, CN_BACKEND_CONNECT_ATTEMPTS) == 0)
    {
        long ival = get_positive_int(value);
        if (ival)
        {
            valid = true;
            monitorSetNetworkTimeout(monitor, MONITOR_CONNECT_ATTEMPTS, ival);
        }
    }
    else
    {
        /** We're modifying module specific parameters and we need to stop the monitor */
        monitorStop(monitor);

        if (monitorRemoveParameter(monitor, key) || value[0])
        {
            /** Either we're removing an existing parameter or adding a new one */
            valid = true;

            if (value[0])
            {
                MXS_CONFIG_PARAMETER p = {};
                p.name = const_cast<char*>(key);
                p.value = const_cast<char*>(value);
                monitorAddParameters(monitor, &p);
            }

            add_monitor_defaults(monitor);
        }

        monitorStart(monitor, monitor->parameters);
    }

    if (valid)
    {
        if (monitor->created_online)
        {
            monitor_serialize(monitor);
        }

        MXS_NOTICE("Updated monitor '%s': %s=%s", monitor->name, key, value);
    }
    else
    {
        runtime_error("Invalid monitor parameter: %s", key);
    }

    spinlock_release(&crt_lock);
    return valid;
}

bool runtime_alter_service(SERVICE *service, const char* zKey, const char* zValue)
{
    string key(zKey);
    string value(zValue);
    bool valid = true;

    spinlock_acquire(&crt_lock);

    if (key == CN_USER)
    {
        serviceSetUser(service, value.c_str(), service->credentials.authdata);
    }
    else if (key == CN_PASSWORD)
    {
        serviceSetUser(service, service->credentials.name, value.c_str());
    }
    else if (key == CN_ENABLE_ROOT_USER)
    {
        serviceEnableRootUser(service, config_truth_value(value.c_str()));
    }
    else if (key == CN_MAX_RETRY_INTERVAL)
    {
        service_set_retry_interval(service, strtol(value.c_str(), NULL, 10));
    }
    else if (key == CN_MAX_CONNECTIONS)
    {
        // TODO: Once connection queues are implemented, use correct values
        serviceSetConnectionLimits(service, strtol(value.c_str(), NULL, 10), 0, 0);
    }
    else if (key == CN_CONNECTION_TIMEOUT)
    {
        serviceSetTimeout(service, strtol(value.c_str(), NULL, 10));
    }
    else if (key == CN_AUTH_ALL_SERVERS)
    {
        serviceAuthAllServers(service, config_truth_value(value.c_str()));
    }
    else if (key == CN_STRIP_DB_ESC)
    {
        serviceStripDbEsc(service, config_truth_value(value.c_str()));
    }
    else if (key == CN_LOCALHOST_MATCH_WILDCARD_HOST)
    {
        serviceEnableLocalhostMatchWildcardHost(service, config_truth_value(value.c_str()));
    }
    else if (key == CN_VERSION_STRING)
    {
        serviceSetVersionString(service, value.c_str());
    }
    else if (key == CN_WEIGHTBY)
    {
        serviceWeightBy(service, value.c_str());
    }
    else if (key == CN_LOG_AUTH_WARNINGS)
    {
        // TODO: Move this inside the service source
        service->log_auth_warnings = config_truth_value(value.c_str());
    }
    else if (key == CN_RETRY_ON_FAILURE)
    {
        serviceSetRetryOnFailure(service, value.c_str());
    }
    else
    {
        runtime_error("Invalid service parameter: %s", key.c_str());
        MXS_ERROR("Unknown parameter for service '%s': %s=%s",
                  service->name, key.c_str(), value.c_str());
        valid = false;
    }

    if (valid)
    {
        service_serialize(service);
        MXS_NOTICE("Updated service '%s': %s=%s", service->name, key.c_str(), value.c_str());
    }

    spinlock_release(&crt_lock);

    return valid;
}

bool runtime_alter_maxscale(const char* name, const char* value)
{
    MXS_CONFIG& cnf = *config_get_global_options();
    string key = name;
    bool rval = false;

    spinlock_acquire(&crt_lock);

    if (key == CN_AUTH_CONNECT_TIMEOUT)
    {
        char* endptr;
        int intval = strtol(value, &endptr, 0);
        if (*endptr == '\0' && intval > 0)
        {
            MXS_NOTICE("Updated '%s' from %d to %d", CN_AUTH_CONNECT_TIMEOUT,
                       cnf.auth_conn_timeout, intval);
            cnf.auth_conn_timeout = intval;
            rval = true;
        }
        else
        {
            runtime_error("Invalid timeout value for '%s': %s", CN_AUTH_CONNECT_TIMEOUT, value);
        }
    }
    else if (key == CN_AUTH_READ_TIMEOUT)
    {
        char* endptr;
        int intval = strtol(value, &endptr, 0);
        if (*endptr == '\0' && intval > 0)
        {
            MXS_NOTICE("Updated '%s' from %d to %d", CN_AUTH_READ_TIMEOUT,
                       cnf.auth_read_timeout, intval);
            cnf.auth_read_timeout = intval;
            rval = true;
        }
        else
        {
            runtime_error("Invalid timeout value for '%s': %s", CN_AUTH_READ_TIMEOUT, value);
        }
    }
    else if (key == CN_AUTH_WRITE_TIMEOUT)
    {
        char* endptr;
        int intval = strtol(value, &endptr, 0);
        if (*endptr == '\0' && intval > 0)
        {
            MXS_NOTICE("Updated '%s' from %d to %d", CN_AUTH_WRITE_TIMEOUT,
                       cnf.auth_write_timeout, intval);
            cnf.auth_write_timeout = intval;
            rval = true;
        }
        else
        {
            runtime_error("Invalid timeout value for '%s': %s", CN_AUTH_WRITE_TIMEOUT, value);
        }
    }
    else if (key == CN_ADMIN_AUTH)
    {
        int boolval = config_truth_value(value);

        if (boolval != -1)
        {
            MXS_NOTICE("Updated '%s' from '%s' to '%s'", CN_ADMIN_AUTH,
                       cnf.admin_auth ? "true" : "false",
                       boolval ? "true" : "false");
            cnf.admin_auth = boolval;
            rval = true;
        }
        else
        {
            runtime_error("Invalid boolean value for '%s': %s", CN_ADMIN_AUTH, value);
        }
    }
    else
    {
        runtime_error("Unknown global parameter: %s", value);
    }

    if (rval)
    {
        config_global_serialize();
    }

    spinlock_release(&crt_lock);

    return rval;
}

bool runtime_create_listener(SERVICE *service, const char *name, const char *addr,
                             const char *port, const char *proto, const char *auth,
                             const char *auth_opt, const char *ssl_key,
                             const char *ssl_cert, const char *ssl_ca,
                             const char *ssl_version, const char *ssl_depth)
{

    if (addr == NULL || strcasecmp(addr, CN_DEFAULT) == 0)
    {
        addr = "::";
    }
    if (port == NULL || strcasecmp(port, CN_DEFAULT) == 0)
    {
        port = "3306";
    }
    if (proto == NULL || strcasecmp(proto, CN_DEFAULT) == 0)
    {
        proto = "MySQLClient";
    }

    if (auth && strcasecmp(auth, CN_DEFAULT) == 0)
    {
        /** Set auth to NULL so the protocol default authenticator is used */
        auth = NULL;
    }

    if (auth_opt && strcasecmp(auth_opt, CN_DEFAULT) == 0)
    {
        /** Don't pass options to the authenticator */
        auth_opt = NULL;
    }

    unsigned short u_port = atoi(port);

    spinlock_acquire(&crt_lock);

    SSL_LISTENER *ssl = NULL;
    bool rval = false;

    if (!serviceHasListener(service, proto, addr, u_port))
    {
        rval = true;

        if (ssl_key && ssl_cert && ssl_ca)
        {
            ssl = create_ssl(name, ssl_key, ssl_cert, ssl_ca, ssl_version, ssl_depth);

            if (ssl == NULL)
            {
                MXS_ERROR("SSL initialization for listener '%s' failed.", name);
                rval = false;
            }
        }

        if (rval)
        {
            const char *print_addr = addr ? addr : "::";
            SERV_LISTENER *listener = serviceCreateListener(service, name, proto, addr,
                                                            u_port, auth, auth_opt, ssl);

            if (listener && listener_serialize(listener) && serviceLaunchListener(service, listener))
            {
                MXS_NOTICE("Created %slistener '%s' at %s:%s for service '%s'",
                           ssl ? "TLS encrypted " : "",
                           name, print_addr, port, service->name);
            }
            else
            {
                MXS_ERROR("Failed to start listener '%s' at %s:%s.", name, print_addr, port);
                rval = false;
            }
        }
    }

    spinlock_release(&crt_lock);
    return rval;
}

bool runtime_destroy_listener(SERVICE *service, const char *name)
{
    bool rval = false;
    char filename[PATH_MAX];
    snprintf(filename, sizeof(filename), "%s/%s.cnf", get_config_persistdir(), name);

    spinlock_acquire(&crt_lock);

    if (unlink(filename) == -1)
    {
        if (errno != ENOENT)
        {
            MXS_ERROR("Failed to remove persisted listener configuration '%s': %d, %s",
                      filename, errno, mxs_strerror(errno));
        }
        else
        {
            rval = false;
            MXS_WARNING("Listener '%s' was not created at runtime. Remove the "
                        "listener manually from the correct configuration file.",
                        name);
        }
    }
    else
    {
        rval = true;
    }

    if (rval)
    {
        rval = serviceStopListener(service, name);

        if (rval)
        {
            MXS_NOTICE("Destroyed listener '%s' for service '%s'. The listener "
                       "will be removed after the next restart of MaxScale.",
                       name, service->name);
        }
        else
        {
            MXS_ERROR("Failed to destroy listener '%s' for service '%s'", name, service->name);
        }
    }

    spinlock_release(&crt_lock);
    return rval;
}

bool runtime_create_monitor(const char *name, const char *module)
{
    spinlock_acquire(&crt_lock);
    bool rval = false;

    if (monitor_find(name) == NULL)
    {
        MXS_MONITOR *monitor = monitor_alloc((char*)name, (char*)module);

        if (monitor)
        {
            /** Mark that this monitor was created after MaxScale was started */
            monitor->created_online = true;
            add_monitor_defaults(monitor);

            if (monitor_serialize(monitor))
            {
                MXS_NOTICE("Created monitor '%s'", name);
                rval = true;
            }
        }
    }
    else
    {
        runtime_error("Can't create monitor '%s', it already exists", name);
    }

    spinlock_release(&crt_lock);
    return rval;
}

bool runtime_destroy_monitor(MXS_MONITOR *monitor)
{
    bool rval = false;
    char filename[PATH_MAX];
    snprintf(filename, sizeof(filename), "%s/%s.cnf", get_config_persistdir(), monitor->name);

    spinlock_acquire(&crt_lock);

    if (unlink(filename) == -1)
    {
        if (errno != ENOENT)
        {
            MXS_ERROR("Failed to remove persisted monitor configuration '%s': %d, %s",
                      filename, errno, mxs_strerror(errno));
        }
        else
        {
            rval = false;
            MXS_WARNING("Monitor '%s' was not created at runtime. Remove the "
                        "monitor manually from the correct configuration file.",
                        monitor->name);
        }
    }
    else
    {
        rval = true;
    }

    if (rval)
    {
        monitorStop(monitor);

        while (monitor->databases)
        {
            monitorRemoveServer(monitor, monitor->databases->server);
        }
        MXS_NOTICE("Destroyed monitor '%s'. The monitor will be removed "
                   "after the next restart of MaxScale.", monitor->name);
    }

    spinlock_release(&crt_lock);
    return rval;
}

static bool extract_relations(json_t* json, set<string>& relations,
                              const char** relation_types,
                              bool (*relation_check)(const string&, const string&))
{
    bool rval = true;

    for (int i = 0; relation_types[i]; i++)
    {
        json_t* arr = mxs_json_pointer(json, relation_types[i]);

        if (arr && json_is_array(arr))
        {
            size_t size = json_array_size(arr);

            for (size_t j = 0; j < size; j++)
            {
                json_t* obj = json_array_get(arr, j);
                json_t* id = json_object_get(obj, CN_ID);
                json_t* type = mxs_json_pointer(obj, CN_TYPE);

                if (id && json_is_string(id) &&
                    type && json_is_string(type))
                {
                    string id_value = json_string_value(id);
                    string type_value = json_string_value(type);

                    if (relation_check(type_value, id_value))
                    {
                        relations.insert(id_value);
                    }
                    else
                    {
                        rval = false;
                    }
                }
                else
                {
                    rval = false;
                }
            }
        }
    }

    return rval;
}

static inline const char* string_or_null(json_t* json, const char* path)
{
    const char* rval = NULL;
    json_t* value = mxs_json_pointer(json, path);

    if (value && json_is_string(value))
    {
        rval = json_string_value(value);
    }

    return rval;
}

/** Check that the body at least defies a data member */
static bool is_valid_resource_body(json_t* json)
{
    return mxs_json_pointer(json, MXS_JSON_PTR_DATA);
}

static bool server_contains_required_fields(json_t* json)
{
    json_t* id = mxs_json_pointer(json, MXS_JSON_PTR_ID);
    json_t* port = mxs_json_pointer(json, MXS_JSON_PTR_PARAM_PORT);
    json_t* address = mxs_json_pointer(json, MXS_JSON_PTR_PARAM_ADDRESS);
    bool rval = false;

    if (!id)
    {
        runtime_error("Request body does not define the '%s' field", MXS_JSON_PTR_ID);
    }
    else if (!json_is_string(id))
    {
        runtime_error("The '%s' field is not a string", MXS_JSON_PTR_ID);
    }
    else if (!address)
    {
        runtime_error("Request body does not define the '%s' field", MXS_JSON_PTR_PARAM_ADDRESS);
    }
    else if (!json_is_string(address))
    {
        runtime_error("The '%s' field is not a string", MXS_JSON_PTR_PARAM_ADDRESS);
    }
    else if (!port)
    {
        runtime_error("Request body does not define the '%s' field", MXS_JSON_PTR_PARAM_PORT);
    }
    else if (!json_is_integer(port))
    {
        runtime_error("The '%s' field is not an integer", MXS_JSON_PTR_PARAM_PORT);
    }
    else
    {
        rval = true;
    }

    return rval;
}

const char* server_relation_types[] =
{
    MXS_JSON_PTR_RELATIONSHIPS_SERVICES,
    MXS_JSON_PTR_RELATIONSHIPS_MONITORS,
    NULL
};

static bool server_relation_is_valid(const string& type, const string& value)
{
    return (type == CN_SERVICES && service_find(value.c_str())) ||
           (type == CN_MONITORS && monitor_find(value.c_str()));
}

static bool unlink_server_from_objects(SERVER* server, set<string>& relations)
{
    bool rval = true;

    for (set<string>::iterator it = relations.begin(); it != relations.end(); it++)
    {
        if (!runtime_unlink_server(server, it->c_str()))
        {
            rval = false;
        }
    }

    return rval;
}

static bool link_server_to_objects(SERVER* server, set<string>& relations)
{
    bool rval = true;

    for (set<string>::iterator it = relations.begin(); it != relations.end(); it++)
    {
        if (!runtime_link_server(server, it->c_str()))
        {
            unlink_server_from_objects(server, relations);
            rval = false;
            break;
        }
    }

    return rval;
}

static string json_int_to_string(json_t* json)
{
    char str[25]; // Enough to store any 64-bit integer value
    int64_t i = json_integer_value(json);
    snprintf(str, sizeof(str), "%ld", i);
    return string(str);
}

SERVER* runtime_create_server_from_json(json_t* json)
{
    SERVER* rval = NULL;

    if (is_valid_resource_body(json) &&
        server_contains_required_fields(json))
    {
        const char* name = json_string_value(mxs_json_pointer(json, MXS_JSON_PTR_ID));
        const char* address = json_string_value(mxs_json_pointer(json, MXS_JSON_PTR_PARAM_ADDRESS));

        /** The port needs to be in string format */
        string port = json_int_to_string(mxs_json_pointer(json, MXS_JSON_PTR_PARAM_PORT));

        /** Optional parameters */
        const char* protocol = string_or_null(json, MXS_JSON_PTR_PARAM_PROTOCOL);
        const char* authenticator = string_or_null(json, MXS_JSON_PTR_PARAM_AUTHENTICATOR);
        const char* authenticator_options = string_or_null(json, MXS_JSON_PTR_PARAM_AUTHENTICATOR_OPTIONS);

        set<string> relations;

        if (extract_relations(json, relations, server_relation_types, server_relation_is_valid) &&
            runtime_create_server(name, address, port.c_str(), protocol, authenticator, authenticator_options))
        {
            rval = server_find_by_unique_name(name);
            ss_dassert(rval);

            if (!link_server_to_objects(rval, relations))
            {
                runtime_destroy_server(rval);
                rval = NULL;
            }
        }
        else
        {
            runtime_error("Invalid relationships in request JSON");
        }
    }
    else
    {
        runtime_error("Missing or bad parameters in request JSON");
    }

    return rval;
}

bool server_to_object_relations(SERVER* server, json_t* old_json, json_t* new_json)
{
    bool rval = false;
    set<string> old_relations;
    set<string> new_relations;

    if (extract_relations(old_json, old_relations, server_relation_types, server_relation_is_valid) &&
        extract_relations(new_json, new_relations, server_relation_types, server_relation_is_valid))
    {
        set<string> removed_relations;
        set<string> added_relations;

        std::set_difference(old_relations.begin(), old_relations.end(),
                            new_relations.begin(), new_relations.end(),
                            std::inserter(removed_relations, removed_relations.begin()));

        std::set_difference(new_relations.begin(), new_relations.end(),
                            old_relations.begin(), old_relations.end(),
                            std::inserter(added_relations, added_relations.begin()));

        if (unlink_server_from_objects(server, removed_relations) &&
            link_server_to_objects(server, added_relations))
        {
            rval = true;
        }
    }

    return rval;
}

bool runtime_alter_server_from_json(SERVER* server, json_t* new_json)
{
    bool rval = false;
    Closer<json_t*> old_json(server_to_json(server, ""));
    ss_dassert(old_json.get());

    if (is_valid_resource_body(new_json) &&
        server_to_object_relations(server, old_json.get(), new_json))
    {
        rval = true;
        json_t* parameters = mxs_json_pointer(new_json, MXS_JSON_PTR_PARAMETERS);
        json_t* old_parameters = mxs_json_pointer(old_json.get(), MXS_JSON_PTR_PARAMETERS);

        ss_dassert(old_parameters);

        if (parameters)
        {
            const char* key;
            json_t* value;

            json_object_foreach(parameters, key, value)
            {
                json_t* new_val = json_object_get(parameters, key);
                json_t* old_val = json_object_get(old_parameters, key);

                if (old_val && new_val && mxs::json_to_string(new_val) == mxs::json_to_string(old_val))
                {
                    /** No change in values */
                }
                else if (!runtime_alter_server(server, key, mxs::json_to_string(value).c_str()))
                {
                    rval = false;
                }
            }
        }
    }
    else
    {
        runtime_error("Missing or bad parameters in request JSON");
    }

    return rval;
}

const char* object_relation_types[] =
{
    MXS_JSON_PTR_RELATIONSHIPS_SERVERS,
    NULL
};

static bool object_relation_is_valid(const string& type, const string& value)
{
    return type == CN_SERVERS && server_find_by_unique_name(value.c_str());
}

/**
 * @brief Do a coarse validation of the monitor JSON
 *
 * @param json JSON to validate
 *
 * @return True of the JSON is valid
 */
static bool validate_monitor_json(json_t* json)
{
    bool rval = false;
    json_t* value;

    if (is_valid_resource_body(json) &&
        (value = mxs_json_pointer(json, MXS_JSON_PTR_ID)) && json_is_string(value) &&
        (value = mxs_json_pointer(json, MXS_JSON_PTR_MODULE)) && json_is_string(value))
    {
        set<string> relations;
        if (extract_relations(json, relations, object_relation_types, object_relation_is_valid))
        {
            rval = true;
        }
    }

    return rval;
}

static bool unlink_object_from_servers(const char* target, set<string>& relations)
{
    bool rval = true;

    for (set<string>::iterator it = relations.begin(); it != relations.end(); it++)
    {
        SERVER* server = server_find_by_unique_name(it->c_str());

        if (!server || !runtime_unlink_server(server, target))
        {
            rval = false;
            break;
        }
    }

    return rval;
}

static bool link_object_to_servers(const char* target, set<string>& relations)
{
    bool rval = true;

    for (set<string>::iterator it = relations.begin(); it != relations.end(); it++)
    {
        SERVER* server = server_find_by_unique_name(it->c_str());

        if (!server || !runtime_link_server(server, target))
        {
            unlink_server_from_objects(server, relations);
            rval = false;
            break;
        }
    }

    return rval;
}

MXS_MONITOR* runtime_create_monitor_from_json(json_t* json)
{
    MXS_MONITOR* rval = NULL;

    if (validate_monitor_json(json))
    {
        const char* name = json_string_value(mxs_json_pointer(json, MXS_JSON_PTR_ID));
        const char* module = json_string_value(mxs_json_pointer(json, MXS_JSON_PTR_MODULE));

        if (runtime_create_monitor(name, module))
        {
            rval = monitor_find(name);
            ss_dassert(rval);

            if (!runtime_alter_monitor_from_json(rval, json))
            {
                runtime_destroy_monitor(rval);
                rval = NULL;
            }
        }
    }
    else
    {
        runtime_error("Missing or bad parameters in request JSON");
    }

    return rval;
}

bool object_to_server_relations(const char* target, json_t* old_json, json_t* new_json)
{
    bool rval = false;
    set<string> old_relations;
    set<string> new_relations;

    if (extract_relations(old_json, old_relations, object_relation_types, object_relation_is_valid) &&
        extract_relations(new_json, new_relations, object_relation_types, object_relation_is_valid))
    {
        set<string> removed_relations;
        set<string> added_relations;

        std::set_difference(old_relations.begin(), old_relations.end(),
                            new_relations.begin(), new_relations.end(),
                            std::inserter(removed_relations, removed_relations.begin()));

        std::set_difference(new_relations.begin(), new_relations.end(),
                            old_relations.begin(), old_relations.end(),
                            std::inserter(added_relations, added_relations.begin()));

        if (unlink_object_from_servers(target, removed_relations) &&
            link_object_to_servers(target, added_relations))
        {
            rval = true;
        }
    }

    return rval;
}

bool runtime_alter_monitor_from_json(MXS_MONITOR* monitor, json_t* new_json)
{
    bool rval = false;
    Closer<json_t*> old_json(monitor_to_json(monitor, ""));
    ss_dassert(old_json.get());

    if (is_valid_resource_body(new_json) &&
        object_to_server_relations(monitor->name, old_json.get(), new_json))
    {
        rval = true;
        bool changed = false;
        json_t* parameters = mxs_json_pointer(new_json, MXS_JSON_PTR_PARAMETERS);
        json_t* old_parameters = mxs_json_pointer(old_json.get(), MXS_JSON_PTR_PARAMETERS);

        ss_dassert(old_parameters);

        if (parameters)
        {
            const char* key;
            json_t* value;

            json_object_foreach(parameters, key, value)
            {
                json_t* new_val = json_object_get(parameters, key);
                json_t* old_val = json_object_get(old_parameters, key);

                if (old_val && new_val && mxs::json_to_string(new_val) == mxs::json_to_string(old_val))
                {
                    /** No change in values */
                }
                else if (runtime_alter_monitor(monitor, key, mxs::json_to_string(value).c_str()))
                {
                    changed = true;
                }
                else
                {
                    rval = false;
                }
            }
        }

        if (changed)
        {
            /** A configuration change was made, restart the monitor */
            monitorStop(monitor);
            monitorStart(monitor, monitor->parameters);
        }
    }
    else
    {
        runtime_error("Missing or bad parameters in request JSON");
    }

    return rval;
}

/**
 * @brief Check if the service parameter can be altered at runtime
 *
 * @param key Parameter name
 * @return True if the parameter can be altered
 */
static bool is_dynamic_param(const string& key)
{
    return key != CN_TYPE &&
           key != CN_ROUTER &&
           key != CN_ROUTER_OPTIONS &&
           key != CN_SERVERS;
}

bool runtime_alter_service_from_json(SERVICE* service, json_t* new_json)
{
    bool rval = false;
    Closer<json_t*> old_json(service_to_json(service, ""));
    ss_dassert(old_json.get());

    if (is_valid_resource_body(new_json) &&
        object_to_server_relations(service->name, old_json.get(), new_json))
    {
        bool changed = false;
        json_t* parameters = mxs_json_pointer(new_json, MXS_JSON_PTR_PARAMETERS);
        json_t* old_parameters = mxs_json_pointer(old_json.get(), MXS_JSON_PTR_PARAMETERS);

        ss_dassert(old_parameters);

        if (parameters)
        {
            /** Create a set of accepted service parameters */
            set<string> paramset;
            for (int i = 0; config_service_params[i]; i++)
            {
                if (is_dynamic_param(config_service_params[i]))
                {
                    paramset.insert(config_service_params[i]);
                }
            }

            rval = true;
            const char* key;
            json_t* value;

            json_object_foreach(parameters, key, value)
            {
                json_t* new_val = json_object_get(parameters, key);
                json_t* old_val = json_object_get(old_parameters, key);

                if (old_val && new_val && mxs::json_to_string(new_val) == mxs::json_to_string(old_val))
                {
                    /** No change in values */
                }
                else if (paramset.find(key) != paramset.end())
                {
                    /** Parameter can be altered */
                    if (!runtime_alter_service(service, key, mxs::json_to_string(value).c_str()))
                    {
                        rval = false;
                    }
                }
            }
        }
    }
    else
    {
        runtime_error("Missing or bad parameters in request JSON");
    }

    return rval;
}

static inline bool is_null_or_bool(json_t* params, const char* name)
{
    bool rval = true;
    json_t* value = mxs_json_pointer(params, name);

    if (value && !json_is_boolean(value))
    {
        runtime_error("Parameter '%s' is not a boolean", name);
        rval = false;
    }

    return rval;
}

static inline bool is_null_or_count(json_t* params, const char* name)
{
    bool rval = true;
    json_t* value = mxs_json_pointer(params, name);

    if (value)
    {
        if (!json_is_integer(value))
        {
            runtime_error("Parameter '%s' is not an integer", name);
            rval = false;
        }
        else if (json_integer_value(value) <= 0)
        {
            runtime_error("Parameter '%s' is not a positive integer", name);
            rval = false;
        }
    }

    return rval;
}

bool validate_logs_json(json_t* json)
{
    json_t* param = mxs_json_pointer(json, MXS_JSON_PTR_PARAMETERS);
    bool rval = false;

    if (param && json_is_object(param))
    {
        rval = is_null_or_bool(param, "highprecision") &&
               is_null_or_bool(param, "maxlog") &&
               is_null_or_bool(param, "syslog") &&
               is_null_or_bool(param, "log_info") &&
               is_null_or_bool(param, "log_warning") &&
               is_null_or_bool(param, "log_notice") &&
               is_null_or_bool(param, "log_debug") &&
               is_null_or_count(param, "throttling/count") &&
               is_null_or_count(param, "throttling/suppress_ms") &&
               is_null_or_count(param, "throttling/window_ms");
    }

    return rval;
}

bool runtime_alter_logs_from_json(json_t* json)
{
    bool rval = false;

    if (validate_logs_json(json))
    {
        json_t* param = mxs_json_pointer(json, MXS_JSON_PTR_PARAMETERS);
        json_t* value;
        rval = true;

        if ((value = mxs_json_pointer(param, "highprecision")))
        {
            mxs_log_set_highprecision_enabled(json_boolean_value(value));
        }

        if ((value = mxs_json_pointer(param, "maxlog")))
        {
            mxs_log_set_maxlog_enabled(json_boolean_value(value));
        }

        if ((value = mxs_json_pointer(param, "syslog")))
        {
            mxs_log_set_syslog_enabled(json_boolean_value(value));
        }

        if ((value = mxs_json_pointer(param, "log_info")))
        {
            mxs_log_set_priority_enabled(LOG_INFO, json_boolean_value(value));
        }

        if ((value = mxs_json_pointer(param, "log_warning")))
        {
            mxs_log_set_priority_enabled(LOG_WARNING, json_boolean_value(value));
        }

        if ((value = mxs_json_pointer(param, "log_notice")))
        {
            mxs_log_set_priority_enabled(LOG_NOTICE, json_boolean_value(value));
        }

        if ((value = mxs_json_pointer(param, "log_debug")))
        {
            mxs_log_set_priority_enabled(LOG_DEBUG, json_boolean_value(value));
        }

        if ((param = mxs_json_pointer(param, "throttling")) && json_is_object(param))
        {
            MXS_LOG_THROTTLING throttle;
            mxs_log_get_throttling(&throttle);

            if ((value = mxs_json_pointer(param, "count")))
            {
                throttle.count = json_integer_value(value);
            }

            if ((value = mxs_json_pointer(param, "suppress_ms")))
            {
                throttle.suppress_ms = json_integer_value(value);
            }

            if ((value = mxs_json_pointer(param, "window_ms")))
            {
                throttle.window_ms = json_integer_value(value);
            }

            mxs_log_set_throttling(&throttle);
        }
    }

    return rval;
}

static bool validate_listener_json(json_t* json)
{
    bool rval = false;
    json_t* param;

    if ((param = mxs_json_pointer(json, MXS_JSON_PTR_ID)) && json_is_string(param) &&
        (param = mxs_json_pointer(json, MXS_JSON_PTR_PARAMETERS)) && json_is_object(param))
    {
        json_t* value;

        if ((value = mxs_json_pointer(param, CN_PORT)) && json_is_integer(value) &&
            (!(value = mxs_json_pointer(param, CN_ADDRESS)) || json_is_string(value)) &&
            (!(value = mxs_json_pointer(param, CN_AUTHENTICATOR)) || json_is_string(value)) &&
            (!(value = mxs_json_pointer(param, CN_AUTHENTICATOR_OPTIONS)) || json_is_string(value)) &&
            (!(value = mxs_json_pointer(param, CN_SSL_KEY)) || json_is_string(value)) &&
            (!(value = mxs_json_pointer(param, CN_SSL_CERT)) || json_is_string(value)) &&
            (!(value = mxs_json_pointer(param, CN_SSL_CA_CERT)) || json_is_string(value)) &&
            (!(value = mxs_json_pointer(param, CN_SSL_VERSION)) || json_is_string(value)) &&
            (!(value = mxs_json_pointer(param, CN_SSL_CERT_VERIFY_DEPTH)) || json_is_integer(value)))
        {
            rval = true;
        }
    }

    return rval;
}

bool runtime_create_listener_from_json(SERVICE* service, json_t* json)
{
    bool rval = false;

    if (validate_listener_json(json))
    {
        string port = json_int_to_string(mxs_json_pointer(json, MXS_JSON_PTR_PARAM_PORT));

        const char* id = string_or_null(json, MXS_JSON_PTR_ID);
        const char* address = string_or_null(json, MXS_JSON_PTR_PARAM_ADDRESS);
        const char* protocol = string_or_null(json, MXS_JSON_PTR_PARAM_PROTOCOL);
        const char* authenticator = string_or_null(json, MXS_JSON_PTR_PARAM_AUTHENTICATOR);
        const char* authenticator_options = string_or_null(json, MXS_JSON_PTR_PARAM_AUTHENTICATOR_OPTIONS);
        const char* ssl_key = string_or_null(json, MXS_JSON_PTR_PARAM_SSL_KEY);
        const char* ssl_cert = string_or_null(json, MXS_JSON_PTR_PARAM_SSL_CERT);
        const char* ssl_ca_cert = string_or_null(json, MXS_JSON_PTR_PARAM_SSL_CA_CERT);
        const char* ssl_version = string_or_null(json, MXS_JSON_PTR_PARAM_SSL_VERSION);
        const char* ssl_cert_verify_depth = string_or_null(json, MXS_JSON_PTR_PARAM_SSL_CERT_VERIFY_DEPTH);

        rval = runtime_create_listener(service, id, address, port.c_str(), protocol,
                                       authenticator, authenticator_options,
                                       ssl_key, ssl_cert, ssl_ca_cert, ssl_version,
                                       ssl_cert_verify_depth);
    }
    else
    {
        runtime_error("Missing or bad parameters in request JSON");
    }

    return rval;
}

json_t* runtime_get_json_error()
{
    json_t* obj = NULL;
    string errmsg = runtime_get_error();

    if (errmsg.length())
    {
        json_t* err = json_object();
        json_object_set_new(err, "detail", json_string(errmsg.c_str()));

        json_t* arr = json_array();
        json_array_append_new(arr, err);

        obj = json_object();
        json_object_set_new(obj, "errors", arr);
    }

    return obj;
}

bool validate_user_json(json_t* json)
{
    bool rval = false;
    json_t* id = mxs_json_pointer(json, MXS_JSON_PTR_ID);
    json_t* type = mxs_json_pointer(json, MXS_JSON_PTR_TYPE);
    json_t* password = mxs_json_pointer(json, MXS_JSON_PTR_PASSWORD);

    if (!id)
    {
        runtime_error("Request body does not define the '%s' field", MXS_JSON_PTR_ID);
    }
    else if (!json_is_string(id))
    {
        runtime_error("The '%s' field is not a string", MXS_JSON_PTR_ID);
    }
    else if (!type)
    {
        runtime_error("Request body does not define the '%s' field", MXS_JSON_PTR_TYPE);
    }
    else if (!json_is_string(type))
    {
        runtime_error("The '%s' field is not a string", MXS_JSON_PTR_TYPE);
    }
    else
    {
        if (strcmp(json_string_value(type), CN_INET) == 0)
        {
            if (!password)
            {
                runtime_error("Request body does not define the '%s' field", MXS_JSON_PTR_PASSWORD);
            }
            else if (!json_is_string(password))
            {
                runtime_error("The '%s' field is not a string", MXS_JSON_PTR_PASSWORD);
            }
            else
            {
                rval = true;
            }
        }
        else if (strcmp(json_string_value(type), CN_UNIX) == 0)
        {
            rval = true;
        }
        else
        {
            runtime_error("Invalid value for field '%s': %s", MXS_JSON_PTR_TYPE,
                          json_string_value(type));
        }
    }

    return rval;
}

bool runtime_create_user_from_json(json_t* json)
{
    bool rval = false;

    if (validate_user_json(json))
    {
        const char* user = json_string_value(mxs_json_pointer(json, MXS_JSON_PTR_ID));
        const char* password = json_string_value(mxs_json_pointer(json, MXS_JSON_PTR_PASSWORD));
        string strtype = json_string_value(mxs_json_pointer(json, MXS_JSON_PTR_TYPE));
        const char* err = NULL;

        if (strtype == CN_INET && (err = admin_add_inet_user(user, password)) == ADMIN_SUCCESS)
        {
            MXS_NOTICE("Create network user '%s'", user);
            rval = true;
        }
        else if (strtype == CN_UNIX && (err = admin_enable_linux_account(user)) == ADMIN_SUCCESS)
        {
            MXS_NOTICE("Enabled account '%s'", user);
            rval = true;
        }
        else if (err)
        {
            runtime_error("Failed to add user '%s': %s", user, err);
        }
    }

    return rval;
}

bool runtime_remove_user(const char* id, enum user_type type)
{
    bool rval = false;
    const char* err = type == USER_TYPE_INET ?
                      admin_remove_inet_user(id, NULL) :
                      admin_disable_linux_account(id);

    if (err == ADMIN_SUCCESS)
    {
        MXS_NOTICE("%s '%s'", type == USER_TYPE_INET ?
                   "Deleted network user" : "Disabled account", id);
        rval = true;
    }
    else
    {
        runtime_error("Failed to remove user '%s': %s", id, err);
    }

    return rval;
}

bool validate_maxscale_json(json_t* json)
{
    bool rval = false;
    json_t* param = mxs_json_pointer(json, MXS_JSON_PTR_PARAMETERS);

    if (param)
    {
        rval = is_null_or_count(param, CN_AUTH_CONNECT_TIMEOUT) &&
               is_null_or_count(param, CN_AUTH_READ_TIMEOUT) &&
               is_null_or_count(param, CN_AUTH_WRITE_TIMEOUT) &&
               is_null_or_bool(param, CN_ADMIN_AUTH);
    }

    return rval;
}

bool ignored_core_parameters(const char* key)
{
    static const char* params[] =
    {
        "libdir",
        "datadir",
        "process_datadir",
        "cachedir",
        "configdir",
        "config_persistdir",
        "module_configdir",
        "piddir",
        "logdir",
        "langdir",
        "execdir",
        "connector_plugindir",
        NULL
    };

    for (int i = 0; params[i]; i++)
    {
        if (strcmp(key, params[i]) == 0)
        {
            return true;
        }
    }

    return false;
}

bool runtime_alter_maxscale_from_json(json_t* new_json)
{
    bool rval = false;

    if (validate_maxscale_json(new_json))
    {
        rval = true;
        json_t* old_json = config_maxscale_to_json("");
        ss_dassert(old_json);

        json_t* new_param = mxs_json_pointer(new_json, MXS_JSON_PTR_PARAMETERS);
        json_t* old_param = mxs_json_pointer(old_json, MXS_JSON_PTR_PARAMETERS);

        const char* key;
        json_t* value;

        json_object_foreach(new_param, key, value)
        {
            json_t* new_val = json_object_get(new_param, key);
            json_t* old_val = json_object_get(old_param, key);

            if (old_val && new_val && mxs::json_to_string(new_val) == mxs::json_to_string(old_val))
            {
                /** No change in values */
            }
            else if (ignored_core_parameters(key))
            {
                /** We can't change these at runtime */
                MXS_DEBUG("Ignoring runtime change to '%s'", key);
            }
            else if (!runtime_alter_maxscale(key, mxs::json_to_string(value).c_str()))
            {
                rval = false;
            }
        }
    }

    return rval;
}
