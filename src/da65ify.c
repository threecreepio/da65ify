#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#ifndef O_BINARY
#define O_BINARY 0
#endif

char bankdata[0x8000];
int bankstartaddr[0x100];

int showHelp(const char *error) {
    if (error != 0) {
        fprintf(stderr, "%s\n\n", error);
    }
    fprintf(stderr, "Usage: da65ify myrom.nes myrom.cdl\
\n\
\nDA65ify converts an NES rom + FCEUX CDL file into a DA65 project.\
\n\
\nParameters:\
\n  <file.nes>              Filename of the ROM file to load\
\n  <file.cdl>              Filename of the CDL file to load\
\n  --banksize <number>     Size of PRG banks, 8=32kb, 4=16kb (default), 2=8kb\
\n  --mlb <path.mlb>        Mesen MLB label file to load\
\n\
\nWhen the program finishes it will create a \"Makefile\" and several \".infofile\"s\
\n'make disassembly' will run the disassembly with da65\
\n'make' will build the NES rom\
\n'make clean' will remove temporary build files\
\n");
    return 2;
}

int reportCDL(FILE *out, int start, int end, int cdl) {
    const char *type = "BYTETABLE";
    if (0b01 == (cdl & 0b01)) {
        type = "CODE";
    }

    fprintf(out, "\
\nRANGE { \
\n  START $%04x; \
\n  END $%04x; \
\n  TYPE %s; \
\n};", start, end, type);
    return 0;
}

struct label {
    unsigned char type;
    int addr;
    int size;
    char *label;
};

struct label *labels = 0;
size_t labelcount = 0;

int parseMLBFile(char *path) {
    FILE *file = fopen(path, "r");
    if (file == 0) return 1;
    char *line = NULL;
    size_t len = 0;

    labelcount = 0;
    size_t maxlabel = 0x4000;
    labels = malloc(sizeof(struct label) * maxlabel);

    while (1) {
        // read next line
        int linelen = getdelim(&line, &len, '\n', file);
        if (linelen <= 3) break;
        line[linelen - 1] = 0; // null terminate line
        char *rest = line;

        // make sure we skip any UTF-8 BOMs
        if (labelcount == 0 && ((uint8_t)rest[0]) == 0xEF && ((uint8_t)rest[1]) == 0xBB && ((uint8_t)rest[2]) == 0xBF) {
            rest += 3;
        }

        char *type = strsep(&rest, ":"); // read label type
        char *addr = strsep(&rest, ":"); // read label rom offset
        char *label = strsep(&rest, ":"); // read label name
        strsep(&rest, ":"); // read comment

        int addrl = (int)strtol(addr, NULL, 16);
        int size = 1;

        if (addr != 0) {
            char *addrend = addr;
            strsep(&addrend, "-");
            if (addrend != 0) {
                size = (int)strtol(addrend, NULL, 16) - addrl;
            }
        }

        // skip ahead this line had no label
        int labellen = strnlen(label, linelen);
        if (labellen <= 0) continue;

        // check if label list needs expanding
        if (labelcount >= maxlabel) {
            // yes, create new list with doubled size
            struct label *newlabels = malloc(sizeof(struct label) * (maxlabel * 2));
            // exit if we could not allocate
            if (newlabels == 0) return 2;
            // copy over previous labels
            memcpy(newlabels, labels, sizeof(struct label) * maxlabel);
            // and free old list
            free(labels);
            // then update to our new label list
            maxlabel *= 2;
            labels = newlabels;
        }

        // add new label
        char *labelstr = malloc(labellen + 1);
        if (labelstr == 0) return 3;
        strncpy(labelstr, label, labellen + 1);
        labels[labelcount].type = type[0];
        labels[labelcount].label = labelstr;
        labels[labelcount].addr = addrl;
        labels[labelcount].size = size;
        labelcount += 1;
    }

    // we've parsed the file, clear memory and exit
    free(line);
    return 0;
}

int writeBankInfo(const char *romfilepath, FILE *romfile, FILE *cdlfile, int banksize, int bank) {
    char bankfilename[0x40];
    sprintf(bankfilename, "bank%d.infofile", bank);

    FILE *out = fopen(bankfilename, "w");
    if (out == 0) return 1;

    int foundstartaddr = 0;
    int startaddr = 0x8000 + ((0x1000 * banksize) * (bank % (8 / banksize)));
    int basestartaddr = startaddr;

    for (int i=0; i<banksize * 0x1000; ++i) {
        int cdl = fgetc(cdlfile);
        bankdata[i] = cdl;
        if (cdl != 0 && foundstartaddr == 0) {
            foundstartaddr = 1;
            int newbank = (cdl >> 2) & 0b11;
            startaddr = 0x8000 + (newbank * 0x2000);
            fprintf(stderr, "bank #%i is mapped to %04x in CDL\n", bank, startaddr);
        }
    }

    if (foundstartaddr == 0) {
        fprintf(stderr, "bank #%i is mapped to %04x\n", bank, startaddr);
    }

    if (startaddr + banksize * 0x1000 - 1 > 0xffff) {
        fprintf(stderr, "bank #%d in cdl is banked into %04x, but with banksize %d it would overflow (to %04x), using %04x instead.\n", bank, startaddr, banksize, startaddr + banksize * 0x1000, basestartaddr);
        startaddr = basestartaddr;
    }

    bankstartaddr[bank] = startaddr;

    fprintf(out, "\
GLOBAL { \
\n  INPUTNAME \"%s\"; \
\n  OUTPUTNAME \"bank%d.asm\"; \
\n  INPUTOFFS $%04x; \
\n  INPUTSIZE $%04x; \
\n  COMMENTS $4; \
\n  STARTADDR $%04x; \
\n  LABELBREAK $1; \
\n};", romfilepath, bank, (banksize * bank * 0x1000) + 0x10, banksize * 0x1000, startaddr);

    // check if an mlb file is in use
    if (labelcount > 0) {
        // if so, find start and end extents of our bank in prg rom
        int start = (banksize * bank * 0x1000);
        int end = (banksize * (bank + 1) * 0x1000);
        // then check every label
        for (size_t i=0; i < labelcount; ++i) {
            // always write ram labels
            if (labels[i].type == 'R') {
                fprintf(out, "\nLABEL { ADDR $%04X; NAME \"%s\"; SIZE $%X; };", labels[i].addr, labels[i].label, labels[i].size);
                continue;
            }

            // on prg rom labels
            if (labels[i].type == 'P') {
                // check so the label belongs to our bank
                if (labels[i].addr < start) continue;
                if (labels[i].addr >= end) continue;
                // and if so, print it.
                fprintf(out, "\nLABEL { ADDR $%04X; NAME \"%s\"; SIZE $%X; };", startaddr + (labels[i].addr - start), labels[i].label, labels[i].size);
                continue;
            }
        }
    }

    int cdl = bankdata[0];
    int istart = 0;
    for (int i=1; i<banksize * 0x1000; ++i) {
        int cdl2 = bankdata[i];
        if ((cdl & 0b11) != (cdl2 & 0b11)) {
            reportCDL(out, startaddr + istart, startaddr + i - 1, cdl);
            istart = i;
        }
        cdl = cdl2;
    }
    reportCDL(out, startaddr + istart, startaddr + banksize * 0x1000 - 1, cdl);
    fclose(out);
    return 0;
}

int main(int argc, char **argv) {
    char *romfilepath = 0;
    char *cdlfilepath = 0;
    char *mlbfilepath = 0;
    struct stat romfilestat;
    struct stat cdlfilestat;
    int banksize = 4;
    int err;

    // optparse
    for (int i=1; i<argc; ++i) {
        if (argv[i][0] == '-') {
            if (i + 1 >= argc) {
                return showHelp(0);
            }
            if (0 == strcmp("--rom", argv[i])) {
                romfilepath = argv[i + 1];
                i += 1;
            } else if (0 == strcmp("--cdl", argv[i])) {
                cdlfilepath = argv[i + 1];
                i += 1;
            } else if (0 == strcmp("--banksize", argv[i])) {
                banksize = atoi(argv[i + 1]);
                i += 1;
            } else if (0 == strcmp("--mlb", argv[i])) {
                mlbfilepath = argv[i + 1];
                i += 1;
            }
        } else if (romfilepath == 0) {
            romfilepath = argv[i];
        } else if (cdlfilepath == 0) {
            cdlfilepath = argv[i];
        } else {
            return showHelp(0);
        }
    }
    if (romfilepath == 0 || cdlfilepath == 0) {
        return showHelp(0);
    }

    // ...
    int romfile = open(romfilepath, O_RDONLY | O_BINARY);
    if (romfile == 0) {
        return showHelp("Could not open ROM file\n");
    }
    err = fstat(romfile, &romfilestat);
    if (0 != err) {
        close(romfile);
        fprintf(stderr, "Could not check ROM file size - %s\n", strerror(errno));
        return -2;
    }

    if (mlbfilepath != 0) {
        if (parseMLBFile(mlbfilepath) != 0) {
            return showHelp(0);
        }
    }

    int cdlfile = open(cdlfilepath, O_RDONLY | O_BINARY);
    if (cdlfile == 0) {
        close(romfile);
        return showHelp(0);
    }
    err = fstat(cdlfile, &cdlfilestat);
    if (0 != err) {
        close(cdlfile);
        close(romfile);
        fprintf(stderr, "Could not check CDL file size - %s\n", strerror(errno));
        return -2;
    }
    if (cdlfilestat.st_size < romfilestat.st_size - 0x10) {
        close(cdlfile);
        close(romfile);
        fprintf(stderr, "CDL file is smaller than ROM\n");
        return -2;
    }
    if (cdlfilestat.st_size != romfilestat.st_size - 0x10) {
        fprintf(stderr, "Warn: CDL file does not match ROM size, that might be bad\n");
    }

    FILE *romf = fdopen(romfile, "r");
    FILE *cdlf = fdopen(cdlfile, "r");
    char header[0x10];
    int headerrlen = fread(header, 1, 0x10, romf);
    if (0x10 != headerrlen) {
        fclose(romf);
        fclose(cdlf);
        fprintf(stderr, "NES file header could not be read %d %x\n", headerrlen, header[5]);
        return -2;
    }
    if (strncmp(header, "NES\x1A", 4) != 0) {
        fclose(romf);
        fclose(cdlf);
        fprintf(stderr, "NES file header invalid %02x\n", header[5]);
        return -2;
    }

    // INES
    FILE *ines = fopen("ines.infofile", "w");
    if (ines == 0) {
        close(cdlfile);
        close(romfile);
        fprintf(stderr, "Could not create ines.infofile\n");
        return -2;
    }

    fprintf(ines, "GLOBAL { \
\n  INPUTNAME \"%s\"; \
\n  OUTPUTNAME \"ines.asm\"; \
\n  INPUTOFFS $0; \
\n  INPUTSIZE $10; \
\n  STARTADDR $0; \
\n}; \
\nRANGE { \
\n  START $0; \
\n  END $10; \
\n  TYPE BYTETABLE; \
\n}; \
", romfilepath);
    fclose(ines);

    int totalPRG = header[0x4];
    int totalBanks = ((header[0x4] / 2) * 8) / banksize;
    for (int i=0; i<totalBanks; ++i) {
        if (0 != writeBankInfo(romfilepath, romf, cdlf, banksize, i)) {
            fclose(romf);
            fclose(cdlf);
            fprintf(stderr, "failed to generate bank %d\n", i);
            return -2;
        }
    }

    fclose(romf);
    fclose(cdlf);
    int chrStart = 0x10 + (0x4000 * totalPRG);
    int chrSize = romfilestat.st_size - chrStart;
    
    // ENTRY
    FILE *entry = fopen("entry.asm", "w");
    if (entry == 0) {
        fprintf(stderr, "Failed to write entry file\n");
        return -2;
    }
    fprintf(entry, ".segment \"INES\"");
    fprintf(entry, "\n.include \"ines.asm\"");
    for (int i=0; i<totalBanks; ++i) {
        fprintf(entry, "\
\n.scope bank%d \
\n.segment \"PRG%d\" \
\n.include \"bank%d.asm\" \
\n.endscope \
\n", i, i, i);
    }

    if (chrSize != 0) {
        fprintf(entry, "\
\n.segment \"CHR\" \
\n.incbin \"%s\", $%04x, $%x \
\n", romfilepath, chrStart, chrSize);
    }
    fclose(entry);


    FILE *layout = fopen("layout", "w");
    if (layout == 0) {
        fprintf(stderr, "Failed to write layout file\n");
        return -2;
    }

    // layout file
    fprintf(layout, "MEMORY {");
    fprintf(layout, "\nINES: start = 0, size = $10;");
    for (int i=0; i<totalBanks; ++i) {
        fprintf(layout, "\nPRG%d: start = $%04x, size = $%04x;", i, bankstartaddr[i], banksize * 0x1000);
    }
    if (chrSize > 0) {
        fprintf(layout, "\nCHR: start = 0, size = $%04x;", chrSize);
    }
    fprintf(layout, "\n}\nSEGMENTS {");
    fprintf(layout, "\nINES: load = INES, type = ro;");
    for (int i=0; i<totalBanks; ++i) {
        fprintf(layout, "\nPRG%d: load = PRG%d, type = ro;", i, i);
    }
    if (chrSize > 0) {
        fprintf(layout, "\nCHR: load = CHR, type = ro;");
    }
    fprintf(layout, "\n}\n");
    fclose(layout);

    // makefile
    FILE *makefile = fopen("Makefile", "w");
    if (makefile == 0) {
        fprintf(stderr, "Failed to write Makefile\n");
        return -2;
    }
    fprintf(makefile, "\n.PHONY: clean");
    fprintf(makefile, "\n");
    fprintf(makefile, "\nbuild: main.nes");
    fprintf(makefile, "\n");
    fprintf(makefile, "\nintegritycheck: main.nes");
    fprintf(makefile, "\n\tradiff2 -x main.nes \"%s\" | head -n 100", romfilepath);
    fprintf(makefile, "\n");
    fprintf(makefile, "\ndisassembly:");
    fprintf(makefile, "\n\tda65 -i ines.infofile");
    for (int i=0; i<totalBanks; ++i) {
        fprintf(makefile, "\n\tda65 -i bank%d.infofile", i);
    }
    fprintf(makefile, "\n");
    fprintf(makefile, "\n%%.o: %%.asm");
    fprintf(makefile, "\n\tca65 --create-dep \"$@.dep\" -g --debug-info $< -o $@");
    fprintf(makefile, "\n");
    fprintf(makefile, "\nmain.nes: layout entry.o");
    fprintf(makefile, "\n\tld65  --dbgfile $@.dbg -C $^ -o $@");
    fprintf(makefile, "\n");
    fprintf(makefile, "\nclean:");
    fprintf(makefile, "\n\trm -f ./main.nes ./*.nes.dbg ./*.o ./*.dep");
    fprintf(makefile, "\n");
    fprintf(makefile, "\ninclude $(wildcard ./*.dep ./*/*.dep)");


    printf("Finished creating project files.\
\n\
\nIf all went well, you should be able to run \"make disassembly\" to create the assembly files\
\nand then \"make\" to build the rom file.\
\n");
    return 0;
}
