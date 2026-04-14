'use strict';

var MAX_BYTES = 64;

function ts() {
  try {
    return (new Date()).toISOString();
  } catch (e) {
    return '' + Date.now();
  }
}

function u32(x) {
  try {
    return x.toInt32() >>> 0;
  } catch (e) {
    return 0;
  }
}

function i32(x) {
  try {
    return x.toInt32();
  } catch (e) {
    return 0;
  }
}

function hexBytes(p, len) {
  if (!p || len <= 0) return '';
  var out = [];
  var n = len < MAX_BYTES ? len : MAX_BYTES;
  var i;
  for (i = 0; i < n; i++) {
    try {
      var b = Memory.readU8(p.add(i));
      out.push(('0' + b.toString(16)).slice(-2));
    } catch (e) {
      out.push('??');
      break;
    }
  }
  return out.join(' ');
}

function scalarList(p, count) {
  var out = [];
  var n = count < 8 ? count : 8;
  var i;
  for (i = 0; i < n; i++) {
    try {
      out.push(ptr(p.add(i * Process.pointerSize)).toString());
    } catch (e) {
      out.push('ERR');
      break;
    }
  }
  return out.join(', ');
}

function log(msg) {
  console.log(ts() + ' pid=' + Process.id + ' ' + msg);
}

function attachExport(name, parser) {
  var addr = null;
  try {
    addr = Module.findGlobalExportByName(name);
  } catch (e) {
    log(name + ' lookup error=' + e);
    return;
  }
  if (!addr) return;

  Interceptor.attach(addr, {
    onEnter: function (args) {
      var info = '';
      try {
        info = parser(args);
      } catch (e) {
        info = 'parser_error=' + e;
      }
      this._info = info;
      log(name + ' enter ' + info);
    },
    onLeave: function (retval) {
      log(name + ' leave kr=' + i32(retval) + ' ' + (this._info || ''));
    }
  });
}

attachExport('IOConnectCallMethod', function (args) {
  var selector = u32(args[1]);
  var scalarCnt = u32(args[3]);
  var structCnt = u32(args[5]);
  return 'selector=' + selector +
    ' scalarCnt=' + scalarCnt +
    ' scalars=[' + scalarList(args[2], scalarCnt) + ']' +
    ' structCnt=' + structCnt +
    ' struct=' + hexBytes(args[4], structCnt);
});

attachExport('IOConnectCallScalarMethod', function (args) {
  var selector = u32(args[1]);
  var scalarCnt = u32(args[3]);
  return 'selector=' + selector +
    ' scalarCnt=' + scalarCnt +
    ' scalars=[' + scalarList(args[2], scalarCnt) + ']';
});

attachExport('IOConnectCallStructMethod', function (args) {
  var selector = u32(args[1]);
  var structCnt = u32(args[3]);
  return 'selector=' + selector +
    ' structCnt=' + structCnt +
    ' struct=' + hexBytes(args[2], structCnt);
});

attachExport('IOConnectCallAsyncMethod', function (args) {
  var selector = u32(args[1]);
  var scalarCnt = u32(args[5]);
  var structCnt = u32(args[7]);
  return 'selector=' + selector +
    ' scalarCnt=' + scalarCnt +
    ' scalars=[' + scalarList(args[4], scalarCnt) + ']' +
    ' structCnt=' + structCnt +
    ' struct=' + hexBytes(args[6], structCnt);
});

attachExport('io_connect_method_scalarI_scalarO', function (args) {
  return 'selector=' + u32(args[1]) +
    ' scalarCnt=' + u32(args[3]) +
    ' scalars=[' + scalarList(args[2], u32(args[3])) + ']';
});

attachExport('io_connect_method_scalarI_structureI', function (args) {
  return 'selector=' + u32(args[1]) +
    ' scalarCnt=' + u32(args[3]) +
    ' scalars=[' + scalarList(args[2], u32(args[3])) + ']' +
    ' structCnt=' + u32(args[5]) +
    ' struct=' + hexBytes(args[4], u32(args[5]));
});

attachExport('io_connect_method_scalarI_structureO', function (args) {
  return 'selector=' + u32(args[1]) +
    ' scalarCnt=' + u32(args[3]) +
    ' scalars=[' + scalarList(args[2], u32(args[3])) + ']';
});

attachExport('io_connect_method_structureI_structureO', function (args) {
  return 'selector=' + u32(args[1]) +
    ' structCnt=' + u32(args[3]) +
    ' struct=' + hexBytes(args[2], u32(args[3]));
});

log('frida-iokit-userclient hooks installed');
