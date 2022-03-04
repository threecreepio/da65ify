# DA65ify

Generate DA65 project files from an NES file and an FCEUX CDL file.

## Usage

Just pass in an NES file and a CDL file to generate the project:

```sh
da65ify myrom.nes myrom.cdl --banksize 4
```

The banksize parameter (which is 4 by default) splits the output into a separate file per bank.
Reasonable options are 1 (4096 bytes), 2 (8192 bytes), 4 (16384 bytes) and 8 (32768 bytes).

Any part of the file that the CDL has not mapped as code is treated as byte tables.

When the program has run successfully it will generate a number of "infofile"s which contain instructions for da65. You can modify that file to add labels and things for different known memory areas. See https://cc65.github.io/doc/da65.html for more information.

When you're ready you can run `make disassembly` (assuming you have make and cc65 installed) to generate assembly files from the infofiles and check out the results, then `make` to build the NES rom.

Once you're finished with the infofiles and are happy with the generated assembly you can safely delete them and the "disassembly" task from the Makefile, and then just work on the assembly files.

## Building

For linux, building should be as easy as cloning the repo and running:

```sh
make
```

Assuming make and gcc are installed.

On windows I recommend using WSL and following the same build instructions, or using WSL and cross-compiling:

```sh
apt-get install gcc-mingw-w64-i686
HOST=i686-w64-mingw32- make
```

Enjoy! /threecreepio
