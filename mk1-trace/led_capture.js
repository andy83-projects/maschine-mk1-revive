// Frida script: capture ALL IOConnectCallMethod calls in NIHardwareAgent
//
// Captures every kext selector call with full payload to find what enables
// rubber LED mode. Run from NIHA startup through Maschine connection + pad press.
//
// Usage:
//   frida -n NIHardwareAgent -l ~/led_capture.js 2>&1 | tee ~/led_capture2.log
//
// Or restart NIHA and attach immediately:
//   frida -n NIHardwareAgent -l ~/led_capture.js --no-pause

'use strict';

var iokit = Process.getModuleByName('IOKit');
var IOConnectCallMethod = iokit.getExportByName('IOConnectCallMethod');
console.log('[init_capture] hooked IOConnectCallMethod at ' + IOConnectCallMethod);

Interceptor.attach(IOConnectCallMethod, {
    onEnter: function(args) {
        var selector = args[1].toInt32();

        var inputScalars    = args[2];
        var inputScalarCnt  = args[3].toInt32();
        var inputStruct     = args[4];
        var inputStructCnt  = args[5].toInt32();

        var scalarsHex = '';
        if (!inputScalars.isNull() && inputScalarCnt > 0) {
            for (var i = 0; i < inputScalarCnt; i++) {
                scalarsHex += '0x' + inputScalars.add(i * 8).readU64().toString(16) + ' ';
            }
        }

        var structHex = '';
        if (!inputStruct.isNull() && inputStructCnt > 0) {
            var arr = new Uint8Array(inputStruct.readByteArray(inputStructCnt));
            for (var j = 0; j < arr.length; j++) {
                structHex += ('00' + arr[j].toString(16)).slice(-2);
                if (j + 1 < arr.length) structHex += ' ';
            }
        }

        var msg = '[sel=' + selector + ']';
        if (scalarsHex) msg += ' scalars=[' + scalarsHex.trim() + ']';
        if (structHex)  msg += ' struct(' + inputStructCnt + ')=' + structHex;
        console.log(msg);
    }
});
