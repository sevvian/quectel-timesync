'use strict';
'require form';
'require uci';

return L.view.extend({
    render: function() {
        var m = new form.Map('quectel-timesync', _('Quectel Time Sync'),
            _('Configure automatic time synchronization from Quectel modem.'));

        // CHANGED: Use NamedSection instead of TypedSection to keep it consistent with init.d
        var s = m.section(form.NamedSection, 'main', 'quectel-timesync', _('General Settings'));
        
        var o;
        o = s.option(form.Flag, 'enabled', _('Enable'));
        o.default = '0';
        o.rmempty = false; // Keep the value in config even if 0

        o = s.option(form.Value, 'path', _('AT Serial Port'), _('e.g. /dev/ttyUSB2'));
        o.rmempty = false;
        o.datatype = 'string';

        o = s.option(form.Value, 'interval', _('Sync Interval (seconds)'));
        o.datatype = 'uinteger';
        o.default = '3600';

        return m.render();
    }
});
