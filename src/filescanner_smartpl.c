/*
 * Copyright (C) 2015 Christian Meffert <christian.meffert@googlemail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>

#include "logger.h"
#include "db.h"
#include "filescanner.h"
#include "misc.h"

#include "SMARTPLLexer.h"
#include "SMARTPLParser.h"
#include "SMARTPL2SQL.h"

static int
smartpl_parse_file(const char *file, struct playlist_info *pli)
{
  pANTLR3_INPUT_STREAM input;
  pSMARTPLLexer lxr;
  pANTLR3_COMMON_TOKEN_STREAM tstream;
  pSMARTPLParser psr;
  SMARTPLParser_playlist_return qtree;
  pANTLR3_COMMON_TREE_NODE_STREAM nodes;
  pSMARTPL2SQL sqlconv;
  SMARTPL2SQL_playlist_return plreturn;
  int ret;

#if ANTLR3C_NEW_INPUT
  input = antlr3FileStreamNew((pANTLR3_UINT8) file, ANTLR3_ENC_8BIT);
#else
  input = antlr3AsciiFileStreamNew((pANTLR3_UINT8) file);
#endif


  // The input will be created successfully, providing that there is enough memory and the file exists etc
  if (input == NULL)
    {
      DPRINTF(E_LOG, L_SCAN, "Unable to open smart playlist file %s\n", file);
      return -1;
    }

  lxr = SMARTPLLexerNew(input);

  // Need to check for errors
  if (lxr == NULL)
    {
      DPRINTF(E_LOG, L_SCAN, "Could not create SMARTPL lexer\n");
      ret = -1;
      goto lxr_fail;
    }

  tstream = antlr3CommonTokenStreamSourceNew(ANTLR3_SIZE_HINT, TOKENSOURCE(lxr));

  if (tstream == NULL)
    {
      DPRINTF(E_LOG, L_SCAN, "Could not create SMARTPL token stream\n");
      ret = -1;
      goto tkstream_fail;
    }

  // Finally, now that we have our lexer constructed, we can create the parser
  psr = SMARTPLParserNew(tstream);  // CParserNew is generated by ANTLR3

  if (tstream == NULL)
    {
      DPRINTF(E_LOG, L_SCAN, "Could not create SMARTPL parser\n");
      ret = -1;
      goto psr_fail;
    }

  qtree = psr->playlist(psr);

  /* Check for parser errors */
  if (psr->pParser->rec->state->errorCount > 0)
    {
      DPRINTF(E_LOG, L_SCAN, "SMARTPL query parser terminated with %d errors\n", psr->pParser->rec->state->errorCount);
      ret = -1;
      goto psr_error;
    }

  DPRINTF(E_DBG, L_SCAN, "SMARTPL query AST:\n\t%s\n", qtree.tree->toStringTree(qtree.tree)->chars);

  nodes = antlr3CommonTreeNodeStreamNewTree(qtree.tree, ANTLR3_SIZE_HINT);
  if (!nodes)
    {
      DPRINTF(E_LOG, L_SCAN, "Could not create node stream\n");
      ret = -1;
      goto psr_error;
    }

  sqlconv = SMARTPL2SQLNew(nodes);
  if (!sqlconv)
    {
      DPRINTF(E_LOG, L_SCAN, "Could not create SQL converter\n");
      ret = -1;
      goto sql_fail;
    }

  plreturn = sqlconv->playlist(sqlconv);

  /* Check for tree parser errors */
  if (sqlconv->pTreeParser->rec->state->errorCount > 0)
    {
      DPRINTF(E_LOG, L_SCAN, "SMARTPL query tree parser terminated with %d errors\n", sqlconv->pTreeParser->rec->state->errorCount);
      ret = -1;
      goto sql_error;
    }

  if (plreturn.title && plreturn.query)
    {
      DPRINTF(E_DBG, L_SCAN, "SMARTPL SQL title '%s' query: -%s-\n", plreturn.title->chars, plreturn.query->chars);

      if (pli->title)
	free(pli->title);
      pli->title = strdup((char *)plreturn.title->chars);

      if (pli->query)
	free(pli->query);
      pli->query = strdup((char *)plreturn.query->chars);

      ret = 0;
    }
  else
    {
      DPRINTF(E_LOG, L_SCAN, "Invalid SMARTPL query\n");
      ret = -1;
    }

 sql_error:
  sqlconv->free(sqlconv);
 sql_fail:
  nodes->free(nodes);
 psr_error:
  psr->free(psr);
 psr_fail:
  tstream->free(tstream);
 tkstream_fail:
  lxr->free(lxr);
 lxr_fail:
  input->close(input);

  return ret;
}

void
scan_smartpl(char *file, time_t mtime)
{
  struct playlist_info *pli;
  int pl_id;
  char virtual_path[PATH_MAX];
  char *ptr;
  int ret;

  /* Fetch or create playlist */
  pli = db_pl_fetch_bypath(file);
  if (!pli)
    {
      pli = (struct playlist_info *) malloc(sizeof(struct playlist_info));
      if (!pli)
	{
	  DPRINTF(E_LOG, L_SCAN, "Out of memory\n");
	  return;
	}

      memset(pli, 0, sizeof(struct playlist_info));

      pli->path = strdup(file);
      snprintf(virtual_path, PATH_MAX, "/file:%s", file);
      ptr = strrchr(virtual_path, '.');
      if (ptr)
	*ptr = '\0';
      pli->virtual_path = strdup(virtual_path);
      pli->type = PL_SMART;
    }
  else
    pl_id = pli->id;

  ret = smartpl_parse_file(file, pli);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_SCAN, "Error parsing smart playlist '%s'\n", file);

      free_pli(pli, 0);
      return;
    }

  if (pli->id)
    {
      ret = db_pl_update(pli);
    }
  else
    {
      ret = db_pl_add(pli, &pl_id);
    }
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_SCAN, "Error adding smart playlist '%s'\n", file);

      free_pli(pli, 0);
      return;
    }

  DPRINTF(E_INFO, L_SCAN, "Added smart playlist as id %d\n", pl_id);

  free_pli(pli, 0);

  DPRINTF(E_INFO, L_SCAN, "Done processing smart playlist\n");
}
