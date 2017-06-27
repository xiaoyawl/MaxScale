module.exports = function() {
    const maxadmin_version = '1.0.0';

    // Common requirements for all subcommands
    this.program = require('commander');
    this.program
        .version(maxadmin_version)
}
