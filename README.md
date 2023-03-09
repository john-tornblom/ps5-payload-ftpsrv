# PS5 FTP Payload
ftps5-payload is a simple FTP server that can be executed on a Playstation 5
that has been jailbroken via the [BD-J entry point][bdj].

## Building
Assuming you have the [ps5-payload-sdk][sdk] installed on a GNU/Linux machine,
the FTP server can be compiled using the following two commands:
```console
john@localhost:ftps5-payload$ export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
john@localhost:ftps5-payload$ make
```

You can also compile the FTP server for POSIX-like systems (tested with Linux):
```console
john@localhost:ftps5-payload$ make -f Makefile.posix
john@localhost:ftps5-payload$ ./ftp-server.elf
```

## Features
Compared to other FTP servers for the PS5, ftps5-payload is multi-threaded,
and supports multiple simultanious connections. Client software that has been
testing include gFTP, Filezilla, and Thunar. Furthermore, the payload supports
a couple of custom SITE commands specifically for the PS5 (executed without
prepending SITE). In particular:
 - KILL - kill the FTP server. This allows you to launch other payloads.
 - MTRW - remount /system and /system_ex with write permissions.

## Limitations
This payload is currently not compatible with the [PS5 webkit exploit][webkit].

## Reporting Bugs
If you encounter problems with ftps5-payload, please [file a github issue][issues].
If you plan on sending pull requests which affect more than a few lines of code,
please file an issue before you start to work on you changes. This will allow us
to discuss the solution properly before you commit time and effort.

## License
ftps5-payload is licensed under the GPLv3+.

[bdj]: https://github.com/john-tornblom/bdj-sdk
[sdk]: https://github.com/john-tornblom/ps5-payload-sdk
[webkit]: https://github.com/Cryptogenic/PS5-IPV6-Kernel-Exploit
[issues]: https://github.com/john-tornblom/ftps5-payload/issues/new
