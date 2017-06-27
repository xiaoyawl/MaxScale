var _ = require('lodash-getpath')
var request = require('request');
var colors = require('colors/safe');
var Table = require('cli-table');

require('./common.js')()

module.exports = function() {
    const maxadmin_version = '1.0.0';

    // Common requirements for all subcommands
    this.program = require('commander');
    this.program
        .version(maxadmin_version)
        .option('-u, --user <username>', 'Username to use [mariadb]', 'mariadb')
        .option('-p, --password <password>', 'Password for the user [admin]', 'admin')
        .option('-h, --host <hostname>', 'The hostname or address where MaxScale is located [localhost]', 'localhost')
        .option('-P, --port <port>', 'The port where MaxScale REST API listens on [8989]', '8989')
        .option('-s, --secure', 'Enable TLS encryption of connections')

    this.getCollection = function (resource, headers, parts) {
        request.get(getUri(resource), function(err, resp, body) {
            if (err) {
                logError(err)
            } else {
                var res = JSON.parse(body)
                var table = getTable(headers)

                res.data.forEach(function(i) {
                    row = []

                    parts.forEach(function(p) {
                        var v = _.getPath(i, p, "")

                        if (Array.isArray(v)) {
                            v = v.join(", ")
                        }
                        row.push(v)
                    })

                    table.push(row)
                })

                console.log(table.toString())
            }
        })
    }

    this.getResource = function (resource, headers, parts) {
        request.get(getUri(resource), function(err, resp, body) {
            if (err) {
                logError(err)
            } else {
                var res = JSON.parse(body)
                var table = getTable(headers)
                table.push(_.pick(res.data, parts))
                console.log(table.toString())
            }
        })
    }

    this.getUri = function(endpoint, options) {
        var base = 'http://';

        if (this.program.secure) {
            base = 'https://';
        }

        return base + this.program.user + ':' + this.program.password + '@' +
            this.program.host + ':' + this.program.port + '/v1/' + endpoint;
    }
}

// Helper function for getting the URI for a MaxScale REST API endpoints

// Creates a table-like array for output. The parameter is an array of header names
function getTable(headobj) {

    for (i = 0; i < headobj.length; i++) {
        headobj[i] = colors.cyan(headobj[i])
    }

    return new Table({
        head: headobj
    })
}

// Log errors
function logError(err) {
    console.log(err)
}
