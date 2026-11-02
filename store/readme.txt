FastFTP360 @VERSION@
A fast FTP server for homebrew Xbox 360 consoles, with network and disk tests.
https://github.com/marcusisdahl/FastFTP360

Start FastFTP360.xex from Aurora like any other app. It is an app, not a
plugin: don't add it to launch.ini.

1. The app opens on the FTP server tab. Press A to start the server.
2. In your FTP client (FileZilla, for example) connect to the IP address shown
   at the top right of the screen: port 21, plain FTP (no TLS), passive mode,
   user name and password xboxftp / xboxftp. Press Y to change the login.
3. When you are done, press B to stop the server, then Back to exit.

Nothing is encrypted, the login included. Use it only on your home network
and never forward its port to the internet. Anyone who logs in can change or
delete files on every drive, the system partitions too.

The network test needs FastFTP360-PC.exe on your PC, from the release zip on
the GitHub page above. FTP doesn't need it.

FastFTP360 is MIT licensed (LICENSE.txt). It isn't affiliated with Microsoft.
