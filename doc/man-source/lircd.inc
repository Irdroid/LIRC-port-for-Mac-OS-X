[NAME]
LIRC daemon - decodes infrared signals and provides them on a Unix
domain socket.

[DESCRIPTION]
The main task of lircd is to decode the infrared signals and provide an
uniform interface for client applications. Clients can connect to lircd
through a Unix domain socket which is located in /dev/lircd. Using this
socket they will get the infrared codes received by lircd and they can
send commands to lircd.

[OPTIONS]
The --permission option gives the file permission of /dev/lircd if it
has to be created in octal representation. Read the documentation for
chmod for further details. If no --permission option is given when the
socket is initially created the default is to give all users read and
write permissions (0666 in octal representation). If /dev/lircd already
exists this option has no effect.

With the --device option you can select the character device which lircd
should read from. The default currently is /dev/lirc but it probably
will change in future.

With the --listen option you can let lircd listen for network
connections on the given port. The default port is 8765. No security
checks are currently implemented.

The --connect option allows you to connect to other lircd servers that
provide a network socket at the given host and port number. The number
of such connections is currently limited to 100.

[FILES]

The config file for lircd is located in /etc/lircd.conf. lircd
has its own log file in /var/log/lircd (beginning with LIRC version
0.6.1 you can configure lircd to use syslogd for log messages; then it
depends on your system configuration where log messages will show up).
You can make lircd reread its config file and reopen its log file by
sending the HUP signal to the program. That way you can rotate old log
files.

