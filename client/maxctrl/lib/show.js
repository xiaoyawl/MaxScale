require('../common.js')()

exports.command = 'show <command>'
exports.desc = 'Show objects'
exports.handler = function() {}
exports.builder = function(yargs) {
    yargs
        .command('server <server>', 'Show server', {}, function(argv) {
            getResource('servers/' + argv.server, [
                {'Server': 'id'},
                {'Address': 'attributes.parameters.address'},
                {'Port': 'attributes.parameters.port'},
                {'Status': 'attributes.status'},
                {'Services': 'relationships.services.data[].id'},
                {'Monitors': 'relationships.monitors.data[].id'},
                {'Master ID': 'attributes.master_id'},
                {'Node ID': 'attributes.node_id'},
                {'Slave Server IDs': 'attributes.slaves'},
                {'Statistics': 'attributes.statistics'}
            ])
        })
        .command('service <service>', 'Show service', {}, function(argv) {
            getResource('services/' + argv.service, [
                {'Service': 'id'},
                {'Router': 'attributes.router'},
                {'Started At': 'attributes.started'},
                {'Current Connections': 'attributes.connections'},
                {'Total Connections': 'attributes.total_connections'},
                {'Servers': 'relationships.servers.data[].id'},
                {'Parameters': 'attributes.parameters'},
                {'Router Diagnostics': 'attributes.router_diagnostics'}
            ])
        })
        .command('monitor <monitor>', 'Show monitor', {}, function(argv) {
            getResource('monitors/' + argv.monitor, [
                {'Monitor': 'id'},
                {'Status': 'attributes.state'},
                {'Servers': 'relationships.servers.data[].id'},
                {'Parameters': 'attributes.parameters'},
                {'Monitor Diagnostics': 'attributes.monitor_diagnostics'}
            ])
        })
        .command('session <session>', 'Show session', {}, function(argv) {
            getResource('sessions/' + argv.session, [
                {'Id': 'id'},
                {'Service': 'relationships.services.data[].id'},
                {'Status': 'attributes.state'},
                {'User': 'attributes.user'},
                {'Host': 'attributes.remote'},
                {'Connected': 'attributes.connected'},
                {'Idle': 'attributes.idle'}
            ])
        })
        .command('filter <filter>', 'Show filter', {}, function(argv) {
            getResource('filters/' + argv.filter, [
                {'Filter': 'id'},
                {'Module': 'attributes.module'},
                {'Services': 'relationships.services.data[].id'},
                {'Parameters': 'attributes.parameters'}
            ])
        })
        .command('module <module>', 'Show loaded module', {}, function(argv) {
            getResource('maxscale/modules/' + argv.module, [
                {'Module': 'id'},
                {'Type': 'attributes.module_type'},
                {'Version': 'attributes.version'},
                {'Maturity': 'attributes.status'},
                {'Description': 'attributes.description'},
                {'Parameters': 'attributes.parameters'},
                {'Commands': 'attributes.commands'}
            ])
        })
    .help()
}
