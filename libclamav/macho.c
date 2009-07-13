/*
 *  Copyright (C) 2009 Sourcefire, Inc.
 *
 *  Authors: Tomasz Kojm <tkojm@clamav.net>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 *  MA 02110-1301, USA.
 */

/* TODO:
 *  - integrate with the matcher
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "clamav.h"
#include "cltypes.h"
#include "others.h"
#include "macho.h"
#include "execs.h"

#define EC32(v, conv)	(conv ? cbswap32(v) : v)
#define EC64(v, conv)	(conv ? cbswap64(v) : v)

struct macho_hdr
{
    uint32_t magic;
    uint32_t cpu_type;
    uint32_t cpu_subtype;
    uint32_t filetype;
    uint32_t ncmds;
    uint32_t sizeofcmds;
    uint32_t flags;
};

struct macho_load_cmd
{
    uint32_t cmd;
    uint32_t cmdsize;
};

struct macho_segment_cmd
{
    char segname[16];
    uint32_t vmaddr;
    uint32_t vmsize;
    uint32_t fileoff;
    uint32_t filesize;
    uint32_t maxprot;
    uint32_t initprot;
    uint32_t nsects;
    uint32_t flags;
};

struct macho_segment_cmd64
{
    char segname[16];
    uint64_t vmaddr;
    uint64_t vmsize;
    uint64_t fileoff;
    uint64_t filesize;
    uint32_t maxprot;
    uint32_t initprot;
    uint32_t nsects;
    uint32_t flags;
};

struct macho_section
{
    char sectname[16];
    char segname[16];
    uint32_t addr;
    uint32_t size;
    uint32_t offset;
    uint32_t align;
    uint32_t reloff;
    uint32_t nreloc;
    uint32_t flags;
    uint32_t res1;
    uint32_t res2;
};

struct macho_section64
{
    char sectname[16];
    char segname[16];
    uint64_t addr;
    uint64_t size;
    uint32_t offset;
    uint32_t align;
    uint32_t reloff;
    uint32_t nreloc;
    uint32_t flags;
    uint32_t res1;
    uint32_t res2;
};

struct macho_thread_state_ppc
{
    uint32_t srr0; /* PC */
    uint32_t srr1;
    uint32_t reg[32];
    uint32_t cr;
    uint32_t xer;
    uint32_t lr;
    uint32_t ctr;
    uint32_t mq;
    uint32_t vrsave;
};

struct macho_thread_state_ppc64
{
    uint64_t srr0; /* PC */
    uint64_t srr1;
    uint64_t reg[32];
    uint32_t cr;
    uint64_t xer;
    uint64_t lr;
    uint64_t ctr;
    uint32_t vrsave;
};

struct macho_thread_state_x86
{
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;
    uint32_t edi;
    uint32_t esi;
    uint32_t ebp;
    uint32_t esp;
    uint32_t ss;
    uint32_t eflags;
    uint32_t eip;
    uint32_t cs;
    uint32_t ds;
    uint32_t es;
    uint32_t fs;
    uint32_t gs;
};

#define RETURN_BROKEN					    \
    if(DETECT_BROKEN) {					    \
	if(ctx->virname)				    \
	    *ctx->virname = "Broken.Executable";	    \
	return cli_checkfp(fd, ctx) ? CL_CLEAN : CL_VIRUS;  \
    }							    \
    return CL_EFORMAT


static uint32_t cli_rawaddr(uint32_t vaddr, struct cli_exe_section *sects, uint16_t nsects, unsigned int *err)
{
	unsigned int i, found = 0;

    for(i = 0; i < nsects; i++) {
	if(sects[i].rva <= vaddr && sects[i].rva + sects[i].vsz > vaddr) {
	    found = 1;
	    break;
	}
    }

    if(!found) {
	*err = 1;
	return 0;
    }

    *err = 0;
    return vaddr - sects[i].rva + sects[i].raw;
}

int cli_scanmacho(int fd, cli_ctx *ctx)
{
	struct macho_hdr hdr;
	struct macho_load_cmd load_cmd;
	struct macho_segment_cmd segment_cmd;
	struct macho_segment_cmd64 segment_cmd64;
	struct macho_section section;
	struct macho_section64 section64;
	unsigned int i, j, sect = 0, conv, m64, nsects;
	unsigned int arch = 0, ep = 0, err;
	struct cli_exe_section *sections = NULL;
	char name[16];

    if(read(fd, &hdr, sizeof(hdr)) != sizeof(hdr)) {
	cli_dbgmsg("cli_scanmacho: Can't read header\n");
	return CL_EFORMAT;
    }

    if(hdr.magic == 0xfeedface) {
	conv = 0;
	m64 = 0;
    } else if(hdr.magic == 0xcefaedfe) {
	conv = 1;
	m64 = 0;
    } else if(hdr.magic == 0xfeedfacf) {
	conv = 0;
	m64 = 1;
    } else if(hdr.magic == 0xcffaedfe) {
	conv = 1;
	m64 = 1;
    } else {
	cli_dbgmsg("cli_scanmacho: Incorrect magic\n");
	return CL_EFORMAT;
    }

    switch(EC32(hdr.cpu_type, conv)) {
	case 7:
	    cli_dbgmsg("MACHO: CPU Type: Intel 32-bit\n");
	    arch = 1;
	    break;
	case 7 | 0x1000000:
	    cli_dbgmsg("MACHO: CPU Type: Intel 64-bit\n");
	    break;
	case 12:
	    cli_dbgmsg("MACHO: CPU Type: ARM\n");
	    break;
	case 14:
	    cli_dbgmsg("MACHO: CPU Type: SPARC\n");
	    break;
	case 18:
	    cli_dbgmsg("MACHO: CPU Type: POWERPC 32-bit\n");
	    arch = 2;
	    break;
	case 18 | 0x1000000:
	    cli_dbgmsg("MACHO: CPU Type: POWERPC 64-bit\n");
	    arch = 3;
	    break;
	default:
	    cli_dbgmsg("MACHO: CPU Type: ** UNKNOWN ** (%u)\n", EC32(hdr.cpu_type, conv));
	    break;
    }

    switch(EC32(hdr.filetype, conv)) {
	case 0x1: /* MH_OBJECT */
	    cli_dbgmsg("MACHO: Filetype: Relocatable object file\n");
	    break;
	case 0x2: /* MH_EXECUTE */
	    cli_dbgmsg("MACHO: Filetype: Executable\n");
	    break;
	case 0x3: /* MH_FVMLIB */
	    cli_dbgmsg("MACHO: Filetype: Fixed VM shared library file\n");
	    break;
	case 0x4: /* MH_CORE */
	    cli_dbgmsg("MACHO: Filetype: Core file\n");
	    break;
	case 0x5: /* MH_PRELOAD */
	    cli_dbgmsg("MACHO: Filetype: Preloaded executable file\n");
	    break;
	case 0x6: /* MH_DYLIB */
	    cli_dbgmsg("MACHO: Filetype: Dynamically bound shared library\n");
	    break;
	case 0x7: /* MH_DYLINKER */
	    cli_dbgmsg("MACHO: Filetype: Dynamic link editor\n");
	    break;
	case 0x8: /* MH_BUNDLE */
	    cli_dbgmsg("MACHO: Filetype: Dynamically bound bundle file\n");
	    break;
	case 0x9: /* MH_DYLIB_STUB */
	    cli_dbgmsg("MACHO: Filetype: Shared library stub for static\n");
	    break;
	default:
	    cli_dbgmsg("MACHO: Filetype: ** UNKNOWN ** (0x%x)\n", EC32(hdr.filetype, conv));
    }

    cli_dbgmsg("MACHO: Number of load commands: %u\n", EC32(hdr.ncmds, conv));
    cli_dbgmsg("MACHO: Size of load commands: %u\n", EC32(hdr.sizeofcmds, conv));

    if(m64)
	lseek(fd, 4, SEEK_CUR);

    hdr.ncmds = EC32(hdr.ncmds, conv);
    if(!hdr.ncmds || hdr.ncmds > 1024) {
	cli_dbgmsg("cli_scanmacho: Invalid number of load commands (%u)\n", hdr.ncmds);
	RETURN_BROKEN;
    }

    for(i = 0; i < hdr.ncmds; i++) {
	if(read(fd, &load_cmd, sizeof(load_cmd)) != sizeof(load_cmd)) {
	    cli_dbgmsg("cli_scanmacho: Can't read load command\n");
	    free(sections);
	    RETURN_BROKEN;
	}
	/*
	if((m64 && EC32(load_cmd.cmdsize, conv) % 8) || (!m64 && EC32(load_cmd.cmdsize, conv) % 4)) {
	    cli_dbgmsg("cli_scanmacho: Invalid command size (%u)\n", EC32(load_cmd.cmdsize, conv));
	    free(sections);
	    RETURN_BROKEN;
	}
	*/
	load_cmd.cmd = EC32(load_cmd.cmd, conv);
	if((m64 && load_cmd.cmd == 0x19) || (!m64 && load_cmd.cmd == 0x01)) { /* LC_SEGMENT */
	    if(m64) {
		if(read(fd, &segment_cmd64, sizeof(segment_cmd64)) != sizeof(segment_cmd64)) {
		    cli_dbgmsg("cli_scanmacho: Can't read segment command\n");
		    free(sections);
		    RETURN_BROKEN;
		}
		nsects = EC32(segment_cmd64.nsects, conv);
		strncpy(name, segment_cmd64.segname, 16);
	    } else {
		if(read(fd, &segment_cmd, sizeof(segment_cmd)) != sizeof(segment_cmd)) {
		    cli_dbgmsg("cli_scanmacho: Can't read segment command\n");
		    free(sections);
		    RETURN_BROKEN;
		}
		nsects = EC32(segment_cmd.nsects, conv);
		strncpy(name, segment_cmd.segname, 16);
	    }
	    name[15] = 0;
	    cli_dbgmsg("MACHO: Segment name: %s\n", name);
	    cli_dbgmsg("MACHO: Number of sections: %u\n", nsects);
	    if(nsects > 255) {
		cli_dbgmsg("cli_scanmacho: Invalid number of sections\n");
		free(sections);
		RETURN_BROKEN;
	    }
	    if(!nsects) {
		cli_dbgmsg("MACHO: ------------------\n");
		continue;
	    }
	    sections = (struct cli_exe_section *) cli_realloc2(sections, (sect + nsects) * sizeof(struct cli_exe_section));
	    if(!sections) {
		cli_errmsg("cli_scanmacho: Can't allocate memory for 'sections'\n");
		return CL_EMEM;
	    }

	    for(j = 0; j < nsects; j++) {
		if(m64) {
		    if(read(fd, &section64, sizeof(section64)) != sizeof(section64)) {
			cli_dbgmsg("cli_scanmacho: Can't read section\n");
			free(sections);
			RETURN_BROKEN;
		    }
		    sections[sect].rva = EC64(section64.addr, conv);
		    sections[sect].vsz = EC64(section64.size, conv);
		    sections[sect].raw = EC32(section64.offset, conv);
		    section64.align = EC32(section64.align, conv);
		    sections[sect].rsz = sections[sect].vsz + (section64.align - (sections[sect].vsz % section64.align)) % section64.align; /* most likely we can assume it's the same as .vsz */
		    strncpy(name, section64.sectname, 16);
		} else {
		    if(read(fd, &section, sizeof(section)) != sizeof(section)) {
			cli_dbgmsg("cli_scanmacho: Can't read section\n");
			free(sections);
			RETURN_BROKEN;
		    }
		    sections[sect].rva = EC32(section.addr, conv);
		    sections[sect].vsz = EC32(section.size, conv);
		    sections[sect].raw = EC32(section.offset, conv);
		    section64.align = EC32(section64.align, conv);
		    sections[sect].rsz = sections[sect].vsz + (section64.align - (sections[sect].vsz % section64.align)) % section64.align;
		    strncpy(name, section.sectname, 16);
		}
		name[15] = 0;
		cli_dbgmsg("MACHO: --- Section %u ---\n", sect);
		cli_dbgmsg("MACHO: Name: %s\n", name);
		cli_dbgmsg("MACHO: Virtual address: 0x%x\n", (unsigned int) sections[sect].rva);
		cli_dbgmsg("MACHO: Virtual size: %u\n", (unsigned int) sections[sect].vsz);
		cli_dbgmsg("MACHO: Raw size: %u\n", (unsigned int) sections[sect].rsz);
		if(sections[sect].raw)
		    cli_dbgmsg("MACHO: File offset: %u\n", (unsigned int) sections[sect].raw);
		sect++;
	    }
	    cli_dbgmsg("MACHO: ------------------\n");

	} else if(arch && (load_cmd.cmd == 0x4 || load_cmd.cmd == 0x5)) { /* LC_(UNIX)THREAD */
	    lseek(fd, 8, SEEK_CUR);
	    switch(arch) {
		case 1: /* x86 */
		{
			struct macho_thread_state_x86 thread_state_x86;

		    if(read(fd, &thread_state_x86, sizeof(thread_state_x86)) != sizeof(thread_state_x86)) {
			cli_dbgmsg("cli_scanmacho: Can't read thread_state_x86\n");
			free(sections);
			RETURN_BROKEN;
		    }
		    break;
		}

		case 2: /* PPC */
		{
			struct macho_thread_state_ppc thread_state_ppc;

		    if(read(fd, &thread_state_ppc, sizeof(thread_state_ppc)) != sizeof(thread_state_ppc)) {
			cli_dbgmsg("cli_scanmacho: Can't read thread_state_ppc\n");
			free(sections);
			RETURN_BROKEN;
		    }
		    ep = EC32(thread_state_ppc.srr0, conv);
		    break;
		}

		case 3: /* PPC64 */
		{
			struct macho_thread_state_ppc64 thread_state_ppc64;

		    if(read(fd, &thread_state_ppc64, sizeof(thread_state_ppc64)) != sizeof(thread_state_ppc64)) {
			cli_dbgmsg("cli_scanmacho: Can't read thread_state_ppc64\n");
			free(sections);
			RETURN_BROKEN;
		    }
		    ep = EC64(thread_state_ppc64.srr0, conv);
		    break;
		}
		default:
		    cli_errmsg("cli_scanmacho: Invalid arch setting!\n");
		    free(sections);
		    return CL_EARG;
	    }
	} else {
	    if(EC32(load_cmd.cmdsize, conv) > sizeof(load_cmd))
		lseek(fd, EC32(load_cmd.cmdsize, conv) - sizeof(load_cmd), SEEK_CUR);
	}
    }

    if(ep) {
	cli_dbgmsg("Entry Point: 0x%x (%u)\n", ep, ep);
	if(sections) {
	    ep = cli_rawaddr(ep, sections, sect, &err);
	    if(err) {
		cli_dbgmsg("cli_scanmacho: Can't calculate EP offset\n");
		free(sections);
		return CL_EFORMAT;
	    }
	    cli_dbgmsg("Entry Point file offset: %u\n", ep);
	}
    }

    free(sections);
    return CL_SUCCESS;
}
