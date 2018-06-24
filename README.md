**hwflush−check** is a tool to check how data is flushed on disk.
hwflush-check tests filesystem consystency under power failure conditions. It does write; fsync; power-cut, and then checks that the data it has written is actully on disk after bootup.

Some description:
https://static.openvz.org/vz-man/man1/pstorage-hwflush-check.1.gz.html

Usage example:
-----
  1. On a server with the hostname test_server, run:
	hwflush-check -l
  2. On a client, run:
    hwflush-check -s test_server -d /mnt/test -t 100
  3. Turn off the client (poweroff button or unplug power cable), and then turn it on again.
  4. Restart the client:
    hwflush-check -s test_server -d /mnt/test -t 100
  5. Check the server output for lines containing the message "cache error detected!"

License: BSD like license
