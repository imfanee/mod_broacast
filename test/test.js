var fd = new File("/tmp/broadcast_js_test.log");
fd.open("w+");
fd.write("JS DTMF binding executed successfully\n");
fd.close();
