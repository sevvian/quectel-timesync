'use strict';
'require form';
'require uci';
'require view';

return view.extend({
    render: function() {
        var m, s, o;

        m = new form.Map('quectel-timesync', _('Quectel Time Sync'),
            _('Configure automatic time synchronization from Quectel modem.'));

        s = m.section(form.NamedSection, 'main', 'quectel-timesync',
            _('General Settings'));

        o = s.option(form.Flag, 'enabled', _('Enable'),
            _('Enable the time sync daemon.'));
        o.default = '0';

        o = s.option(form.Value, 'path', _('AT Serial Port'),
            _('Exact serial port for AT commands, e.g., /dev/ttyUSB2. This is required.'));
        o.rmempty = false;
        o.placeholder = '/dev/ttyUSB2';

        o = s.option(form.Value, 'interval', _('Sync Interval (seconds)'),
            _('How often to query the network time. Minimum 10 seconds.'));
        o.datatype = 'uinteger';
        o.placeholder = '3600';

        return m.render();
    }
});
