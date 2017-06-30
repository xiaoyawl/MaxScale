module.exports = function() {
    const maxctrl_version = '1.0.0';

    // Common requirements for all subcommands
    this.program = require('commander');
    this.program
        .version(maxctrl_version)
}
