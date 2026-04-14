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

function log(msg) {
  console.log(ts() + ' pid=' + Process.id + ' ' + msg);
}

function attachGlobal(name, parser) {
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

attachGlobal('IOServiceOpen', function (args) {
  return 'owningTask=' + args[1] + ' type=' + u32(args[2]);
});

attachGlobal('IOConnectMapMemory64', function (args) {
  return 'memoryType=' + u32(args[1]) + ' options=' + u32(args[4]);
});

attachGlobal('IOHIDManagerOpen', function (args) {
  return 'options=' + u32(args[1]);
});

attachGlobal('IOHIDDeviceOpen', function (args) {
  return 'options=' + u32(args[1]);
});

attachGlobal('IOHIDDeviceSetReport', function (args) {
  var reportType = u32(args[1]);
  var reportId = u32(args[2]);
  var reportLen = u32(args[4]);
  return 'reportType=' + reportType +
    ' reportId=' + reportId +
    ' len=' + reportLen +
    ' data=' + hexBytes(args[3], reportLen);
});

attachGlobal('IOHIDDeviceGetReport', function (args) {
  var reportType = u32(args[1]);
  var reportId = u32(args[2]);
  var reportLenPtr = args[4];
  var reportLen = 0;
  try {
    reportLen = Memory.readU32(reportLenPtr);
  } catch (e) {
    reportLen = 0;
  }
  return 'reportType=' + reportType +
    ' reportId=' + reportId +
    ' len=' + reportLen;
});

log('frida-hid hooks installed');
