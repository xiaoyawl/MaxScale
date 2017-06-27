require('./common.js')()

'use strict';

program
    .command(require('./lib/list.js'))
    .command(require('./lib/show.js'))
    .demandCommand(1, 'At least one command is required')
    .argv
