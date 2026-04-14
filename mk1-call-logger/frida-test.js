'use strict';

function log(s) {
  console.log(s);
}

var addr = null;

try {
  addr = Module.findGlobalExportByName('IOConnectCallMethod');
  log('findGlobalExportByName addr=' + addr);
} catch (e) {
  log('findGlobalExportByName error=' + e);
}

if (addr) {
  try {
    Interceptor.attach(addr, {
      onEnter: function (args) {
        log('IOConnectCallMethod enter selector=' + (args[1].toInt32() >>> 0));
      },
      onLeave: function (retval) {
        log('IOConnectCallMethod leave kr=' + retval.toInt32());
      }
    });
    log('installed');
  } catch (e) {
    log('attach error=' + e);
  }
} else {
  log('no addr');
}
