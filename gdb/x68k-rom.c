/* Remote debugging using X68000 IPL rom debugger v1.0.

   Copyright 2012 Free Software Foundation, Inc.

   This file is part of GDB.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

#include "defs.h"
#include "gdbcore.h"
#include "target.h"
#include "monitor.h"
#include "gdb_string.h"
#include "serial.h"
#include "regcache.h"
#include "command.h"

static char *hudsonbug_inits[] =
{"\r", NULL};

static struct target_ops hudsonbug_ops;
static struct monitor_ops hudsonbug_cmds;

static char *hudsonbug_regnames[] = {
  "D0", "D1", "D2", "D3", "D4", "D5", "D6", "D7",
  "A0", "A1", "A2", "A3", "A4", "A5", "A6", "A7",
  "SR", "PC"
};

struct hudsonbug_upload_context {
  unsigned long addr;		/* The address to start uploading */
  unsigned long count;		/* Uploaded data count */
};

static void
hudsonbug_open (char *args, int from_tty)
{
  monitor_open (args, &hudsonbug_cmds, from_tty);
}

/*
 * Send section content to monitor
 */

static void
hudsonbug_load_section (bfd *abfd, asection *s, void *obj)
{
  struct hudsonbug_upload_context *ctx =
    (struct hudsonbug_upload_context *) obj;
  bfd_size_type section_size;
  bfd_vma section_base;
  unsigned short buf;
  int i;

  if (s->flags & SEC_LOAD)
    {
      section_size = bfd_section_size (abfd, s);
      section_base = bfd_section_lma (abfd, s) + (bfd_vma) ctx->addr;

      ctx->count += section_size;

      printf_filtered ("Loading section %s, size 0x%lx lma ",
                       bfd_section_name (abfd, s),
                       (unsigned long) section_size);
      fputs_filtered (paddress (target_gdbarch, section_base), gdb_stdout);
      printf_filtered ("\n");

      gdb_flush (gdb_stdout);

      /* Enter edit mode */
      monitor_printf ("e %x\r", section_base);

      /* Upload 2 bytes a time */
      for (i = 0; i < section_size; i += 2)
        {
          monitor_expect (":", NULL, 0);
	  bfd_get_section_contents (abfd, s, (char *) &buf, i, 2);
	  monitor_printf ("%x\n", buf);
	}

      monitor_expect (":", NULL, 0);
      monitor_printf ("\003");
      monitor_expect_prompt (NULL, 0);
    }
}

/*
 * "upload" command handler
 */

static void
hudsonbug_upload_command (char *args, int from_tty)
{
  char **argv;
  char *filename;
  long addr;
  bfd *abfd;
  struct hudsonbug_upload_context ctx;

  argv = gdb_buildargv (args);

  filename = argv[0];
  /* Base address (raw binary/xfile) */
  addr = strtoul (argv[1], &argv[2], 16);

  if (filename == NULL || filename[0] == 0)
    filename = get_exec_file (1);	/* Current executable */

  abfd = bfd_openr (filename, 0);
  if (!abfd)
    error (_("Unable to open file %s."), filename);
  if (bfd_check_format (abfd, bfd_object) == 0)
    error (_("File is not an object file."));

  ctx.addr = addr;
  ctx.count = 0;

  bfd_map_over_sections (abfd, hudsonbug_load_section, &ctx);

  /* TODO : Set PC ? */
}

/* Output :
+x
PC=00FF0D3C USP=00000000 SSP=00001FFC SR=2000 X:0  N:0  Z:0  V:0  C:0
D  00000000 FFFF9470 00000007 00000009  00000001 00001206 00FF00E0 00004AB9
A  00000CB0 00000000 00FF00E1 000012AD  00001120 00001206 00001000 00001FFC
+
*/

static int
hudsonbug_dumpregs (struct regcache *regcache)
{
  struct gdbarch *gdbarch = get_regcache_arch (regcache);
  char buf[1024];
  int resp_len;
  char *p;

  /* Send the dump register command to the monitor and
     get the reply.  */
  monitor_printf (hudsonbug_cmds.dump_registers);
  resp_len = monitor_expect_prompt (buf, sizeof (buf));

  p = strtok (buf, " \r\n");
  while (p)
    {
      if (strncmp (p, "PC", 2) == 0)
        {
          p += 3;
          monitor_supply_register (regcache, 17, p);
	}
      else if (strncmp (p, "SR", 2) == 0)
        {
	  p += 3;
	  monitor_supply_register (regcache, 16, p);
	}
      else if ((p[0] == 'D') || (p[0] == 'A'))
        {
	  int i = 0;
	  int rn = p[0] == 'D' ? 0 : 8;

	  while (i < 8)
	    {
              p = strtok (NULL, " \r\n");
              if (p)
	        monitor_supply_register (regcache, rn + i, p);
              i++;
	    }
	}
      p = strtok (NULL, " \r\n");
    }

  return 0;
}

static void
init_hudsonbug_cmds (void)
{
  hudsonbug_cmds.flags = MO_GETMEM_NEEDS_RANGE | MO_SETMEM_INTERACTIVE | 
  			 MO_HAS_BLOCKWRITES | MO_EXACT_DUMPADDR |
			 MO_ADDR_RANGE_INCLUSIVE;

  hudsonbug_cmds.init = hudsonbug_inits;	/* Init strings		*/
  hudsonbug_cmds.cont = "g\r";			/* continue command	*/
  hudsonbug_cmds.step = "t\r";			/* single step		*/
  hudsonbug_cmds.stop = NULL;			/* interrupt command	*/
  hudsonbug_cmds.set_break = "b %x\r";		/* set a breakpoint	*/
  hudsonbug_cmds.clr_break = "bc %x\r";		/* clear a breakpoint	*/
  hudsonbug_cmds.clr_all_break = "br\r";	/* clear all breakpoints */
  hudsonbug_cmds.fill = "f %x %x %x\r";		/* fill (start count val) */
  hudsonbug_cmds.setmem.cmdw = "e %x\r";	/* setmem.cmdb (addr) */
  hudsonbug_cmds.setmem.resp_delim = ":";	/* setmem.resp_delim */
  hudsonbug_cmds.setmem.term_cmd = "\003";	/* Escape from setmem sub */
  hudsonbug_cmds.getmem.cmdb = "d %x %x\r";	/* getmem.cmdb (start addr,
						   end addr) 		*/
  hudsonbug_cmds.getmem.resp_delim = " ";	/* getmem.resp_delim	*/
  hudsonbug_cmds.setreg.cmd = "x%s %x\r";	/* setreg.cmd (name, value) */
  hudsonbug_cmds.getreg.cmd = NULL;		/* getreg.cmd (name)	*/
  hudsonbug_cmds.getreg.resp_delim = NULL;	/* getreg.resp_delim	*/
  hudsonbug_cmds.getreg.term_cmd = NULL;	/* Escape from getreg	*/
  hudsonbug_cmds.dump_registers = "x\r";	/* dump_registers	*/
  hudsonbug_cmds.dumpregs = hudsonbug_dumpregs;	/* dump registers parser */
  hudsonbug_cmds.register_pattern = NULL;	/* register_pattern */
  hudsonbug_cmds.load_routine = NULL;		/* Download routine */
  hudsonbug_cmds.load = NULL;			/* download command	*/
  hudsonbug_cmds.prompt = "+";			/* monitor command prompt */
  hudsonbug_cmds.line_term = "\r";		/* end-of-line terminator */
  hudsonbug_cmds.target = &hudsonbug_ops;	/* target operations	*/
  hudsonbug_cmds.stopbits = SERIAL_1_STOPBITS;	/* number of stop bits	*/
  hudsonbug_cmds.regnames = hudsonbug_regnames;	/* registers names	*/
  hudsonbug_cmds.num_breakpoints = 10;		/* number of breakpoints */
  hudsonbug_cmds.magic = MONITOR_OPS_MAGIC;	/* magic		*/
}

void
_initialize_hudsonbug_rom ()
{
  int i;

  init_hudsonbug_cmds ();
  init_monitor_ops (&hudsonbug_ops);
  hudsonbug_ops.to_shortname = "hudsonbug";
  hudsonbug_ops.to_longname = "HudsonSoft bug monitor for X68000 series";
  hudsonbug_ops.to_doc = "Debug via the HudsonSoft bug monitor.\n\
Specify the serial device it is connected to (e.g. /dev/ttya).";
  hudsonbug_ops.to_open = hudsonbug_open;

  add_target (&hudsonbug_ops);

  add_com ("upload", class_obscure, hudsonbug_upload_command, _("\
Upload a binary file via the IPL rom debugger to target memory."));
}
